#pragma once

// Realtime 3D atmospheric microclimate solver.
//
// A collocated Boussinesq incompressible model on a Cartesian grid that wraps the
// terrain heightmap as a stair-step solid boundary. It is intentionally a CPU,
// multithreaded solver so it slots into the existing all-CPU pipeline with no compute
// scaffolding. Every visible phenomenon (orographic cloud, slope wind, rain shadow,
// valley cold pool) emerges from the coupled equations rather than being painted.

#include "common.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct WeatherParams {
    float windSpeed = 7.0f;       // m/s prevailing inflow
    float windDirDeg = 20.0f;     // 0 -> blowing toward +x
    float inflowRH = 0.75f;       // relative humidity of incoming air
    float surfaceTempC = 14.0f;   // base surface temperature
    float thetaLapse = 0.0050f;   // K/m potential-temperature gradient (stability)
    float solarHeating = 0.010f;  // K/s surface heating at full incidence
    float irCooling = 0.0035f;    // K/s longwave surface cooling
    float surfaceDrag = 0.8f;     // 1/s friction in the surface layer
    float windNudge = 0.18f;      // 1/s relaxation of interior wind toward prevailing
    float rainFall = 6.0f;        // m/s rain terminal velocity
    float autoconv = 0.0016f;     // 1/s cloud -> rain conversion
    float autoThresh = 0.0001f;   // kg/kg cloud water threshold for conversion
    float rainEvap = 0.25f;       // rain evaporation scale in subsaturated air
    float buoyancy = 1.0f;        // buoyancy multiplier
    int pressureIters = 24;       // Jacobi pressure iterations
    float timeScale = 45.0f;      // sim seconds per real second
    float vScale = 14.0f;         // atmosphere-depth scale: physical altitude = renderHeight * vScale
                                  // (box renders at 165 m but the modeled column is deeper so
                                  //  adiabatic cooling on lift is strong enough to form cloud)
};

enum class WeatherField {
    Temperature,
    ThetaPert,
    VerticalWind,
    WindSpeed,
    Vapor,
    RelHumidity,
    Cloud,
    Rain,
    Vorticity,
};

class Weather {
public:
    ~Weather() { stopWorker(); }

    void configure(int nx, int ny, int nz, float baseY, float lidY)
    {
        std::lock_guard<std::mutex> lk(stepMtx_);
        std::lock_guard<std::mutex> lk2(histMtx_);
        history_.clear();
        viewSnap_.reset();
        nx_ = std::max(8, nx);
        ny_ = std::max(8, ny);
        nz_ = std::max(8, nz);
        baseY_ = baseY;
        lidY_ = std::max(lidY, baseY + 20.0f);
        dx_ = kTerrainWorldSize / static_cast<float>(nx_);
        dz_ = kTerrainWorldSize / static_cast<float>(nz_);
        dy_ = (lidY_ - baseY_) / static_cast<float>(ny_);
        const std::size_t n = cellCount();
        u_.assign(n, 0.0f); v_.assign(n, 0.0f); w_.assign(n, 0.0f);
        theta_.assign(n, 0.0f); qv_.assign(n, 0.0f); qc_.assign(n, 0.0f); qr_.assign(n, 0.0f);
        u2_.assign(n, 0.0f); v2_.assign(n, 0.0f); w2_.assign(n, 0.0f);
        s2_.assign(n, 0.0f);
        // p_ carries one extra "zero" slot at index n: the precomputed Poisson stencil
        // points excluded (solid/boundary) neighbours at it so the SOR sweep can gather
        // all six neighbours branchlessly.
        p_.assign(n + 1, 0.0f); div_.assign(n, 0.0f);
        solid_.assign(n, 0u);
        poissonDirty_ = true;
        colHeight_.assign(static_cast<std::size_t>(nx_) * nz_, baseY_);
        surfaceJ_.assign(static_cast<std::size_t>(nx_) * nz_, 0);
        precipRate_.assign(static_cast<std::size_t>(nx_) * nz_, 0.0f);
        precipAccum_.assign(static_cast<std::size_t>(nx_) * nz_, 0.0f);
        // Render thread only reads the latest finished snapshot, so keep a tiny ring
        // (a couple of frames of slack so a snapshot is never freed while being read).
        maxHistory_ = 3;
        configured_ = true;
    }

    // colHeights: world Y of the terrain surface per column, size nx*nz (column-major i + k*nx).
    void setColumnHeights(const std::vector<float>& colHeights)
    {
        std::lock_guard<std::mutex> lk(stepMtx_);
        if (!configured_ || colHeights.size() != colHeight_.size()) return;
        colHeight_ = colHeights;
        rebuildMask();
    }

    void reset(const WeatherParams& p)
    {
        std::lock_guard<std::mutex> lk(stepMtx_);
        if (!configured_) return;
        vScale_ = p.vScale;
        glm::vec3 wind = windVector(p);
        for (int k = 0; k < nz_; ++k)
            for (int j = 0; j < ny_; ++j)
                for (int i = 0; i < nx_; ++i) {
                    int c = idx(i, j, k);
                    float th = thetaEnv(j, p);
                    theta_[c] = th;
                    float T = th * exner(cellY(j));
                    qv_[c] = p.inflowRH * qSat(T, pressureAt(cellY(j)));
                    qc_[c] = 0.0f; qr_[c] = 0.0f;
                    if (solid_[c]) { u_[c] = v_[c] = w_[c] = 0.0f; }
                    else { u_[c] = wind.x; v_[c] = 0.0f; w_[c] = wind.z; }
                }
        std::fill(precipRate_.begin(), precipRate_.end(), 0.0f);
        std::fill(precipAccum_.begin(), precipAccum_.end(), 0.0f);
        simTime_ = 0.0f;
        auto snap = makeSnapshot();
        {
            std::lock_guard<std::mutex> lk(histMtx_);
            history_.clear();
            history_.push_back(snap);
            viewSnap_ = snap;
        }
    }

    // ---- background worker ---------------------------------------------------------------
    void startWorker()
    {
        if (alive_.load()) return;
        startPool();
        alive_.store(true);
        worker_ = std::thread([this] { workerLoop(); });
    }
    void stopWorker()
    {
        alive_.store(false);
        if (worker_.joinable()) worker_.join();
        stopPool();
    }
    // Called from the render thread every frame to hand the solver its inputs.
    void setControls(bool running, const WeatherParams& p, const glm::vec3& sunDir)
    {
        std::lock_guard<std::mutex> lk(ctrlMtx_);
        ctrlRunning_ = running;
        ctrlParams_ = p;
        ctrlSun_ = sunDir;
    }

    // ---- rendering / probing accessors -------------------------------------------------
    bool ready() const { return configured_; }
    int nx() const { return nx_; }
    int ny() const { return ny_; }
    int nz() const { return nz_; }
    float simTime() const { return simTime_; }

    glm::vec3 cellCenter(int i, int j, int k) const
    {
        return {-kTerrainWorldSize * 0.5f + (i + 0.5f) * dx_, cellY(j), -kTerrainWorldSize * 0.5f + (k + 0.5f) * dz_};
    }
    bool isSolid(int i, int j, int k) const { return solid_[idx(i, j, k)] != 0u; }
    int surfaceCell(int i, int k) const { return surfaceJ_[static_cast<std::size_t>(k) * nx_ + i]; }
    glm::vec3 velocity(int i, int j, int k) const { int c = idx(i, j, k); return {aU()[c], aV()[c], aW()[c]}; }
    float cloudAt(int i, int j, int k) const { return aQc()[idx(i, j, k)]; }
    float rainAt(int i, int j, int k) const { return aQr()[idx(i, j, k)]; }

    // ---- history / scrubber (render thread) ----------------------------------------------
    int historyCount() const
    {
        std::lock_guard<std::mutex> lk(histMtx_);
        return static_cast<int>(history_.size());
    }
    // Select which snapshot the read accessors return (-1 = latest / live). Grabs an owning
    // pointer so the snapshot stays alive while the overlay reads it, even if the worker
    // rolls the ring buffer forward.
    void setViewFrame(int f)
    {
        std::lock_guard<std::mutex> lk(histMtx_);
        if (history_.empty()) { viewSnap_.reset(); viewingLive_ = true; return; }
        int n = static_cast<int>(history_.size());
        if (f < 0 || f >= n) { viewSnap_ = history_.back(); viewingLive_ = true; }
        else { viewSnap_ = history_[f]; viewingLive_ = false; }
    }
    bool viewingLive() const { return viewingLive_; }
    float frameSimTime(int f) const
    {
        std::lock_guard<std::mutex> lk(histMtx_);
        if (history_.empty()) return 0.0f;
        int n = static_cast<int>(history_.size());
        if (f < 0 || f >= n) return history_.back()->simTime;
        return history_[f]->simTime;
    }
    float precipRate(int i, int k) const { return precipRate_[static_cast<std::size_t>(k) * nx_ + i]; }
    float precipAccum(int i, int k) const { return precipAccum_[static_cast<std::size_t>(k) * nx_ + i]; }

    float sample(WeatherField f, int i, int j, int k, const WeatherParams& p) const
    {
        int c = idx(i, j, k);
        float y = cellY(j);
        const auto& U = aU(); const auto& V = aV(); const auto& W = aW();
        const auto& TH = aTheta(); const auto& QV = aQv(); const auto& QC = aQc(); const auto& QR = aQr();
        switch (f) {
        case WeatherField::Temperature: return TH[c] * exner(y) - 273.15f;
        case WeatherField::ThetaPert: return TH[c] - thetaEnv(j, p);
        case WeatherField::VerticalWind: return V[c];
        case WeatherField::WindSpeed: return std::sqrt(U[c] * U[c] + V[c] * V[c] + W[c] * W[c]);
        case WeatherField::Vapor: return QV[c] * 1000.0f;
        case WeatherField::RelHumidity: {
            float T = TH[c] * exner(y);
            float qs = qSat(T, pressureAt(y));
            return qs > 1e-9f ? glm::clamp(QV[c] / qs, 0.0f, 1.4f) : 0.0f;
        }
        case WeatherField::Cloud: return QC[c] * 1000.0f;
        case WeatherField::Rain: return QR[c] * 1000.0f;
        case WeatherField::Vorticity: return vorticityMag(i, j, k);
        }
        return 0.0f;
    }

    // Build a per-column precipitation weight grid for erosion coupling (copy under lock so
    // the worker isn't mid-update).
    std::vector<float> precipWeights() const
    {
        std::lock_guard<std::mutex> lk(stepMtx_);
        return precipAccum_;
    }
    void clearPrecipAccum()
    {
        std::lock_guard<std::mutex> lk(stepMtx_);
        std::fill(precipAccum_.begin(), precipAccum_.end(), 0.0f);
    }

private:
    static constexpr float kG = 9.81f;
    static constexpr float kCp = 1004.0f;
    static constexpr float kRd = 287.0f;
    static constexpr float kL = 2.5e6f;
    static constexpr float kP0 = 1.0e5f;

    int nx_ = 0, ny_ = 0, nz_ = 0;
    float dx_ = 1, dy_ = 1, dz_ = 1;
    float baseY_ = 0, lidY_ = 1;
    float vScale_ = 6.0f;
    bool configured_ = false;
    float simTime_ = 0.0f;

    std::vector<float> u_, v_, w_, theta_, qv_, qc_, qr_;
    std::vector<float> u2_, v2_, w2_, s2_, p_, div_;
    std::vector<std::uint8_t> solid_;

    // Precomputed Poisson stencil for the pressure solve. The linear system only changes
    // when the terrain (solid mask) changes, so the per-cell neighbour indices, inverse
    // diagonal, and red/black colour lists are built once and reused across every SOR
    // sweep of every step instead of being recomputed ~48x/step.
    std::vector<int> nbXm_, nbXp_, nbYm_, nbYp_, nbZm_, nbZp_;
    std::vector<float> invDen_;
    std::vector<int> redCells_, blackCells_;
    bool poissonDirty_ = true;
    std::vector<float> colHeight_;
    std::vector<int> surfaceJ_;
    std::vector<float> precipRate_, precipAccum_;

    // Rolling history for the timeline scrubber. Snapshots are immutable once published, so
    // the render thread can read one (held alive by shared_ptr) while the worker keeps stepping.
    struct Snapshot {
        std::vector<float> u, v, w, theta, qv, qc, qr;
        float simTime = 0.0f;
    };
    std::deque<std::shared_ptr<const Snapshot>> history_;
    std::shared_ptr<const Snapshot> viewSnap_; // render thread: current snapshot to read
    bool viewingLive_ = true;
    int maxHistory_ = 120;

    // Persistent worker pool for parallelFor. The old implementation spawned and joined
    // a fresh set of std::threads on EVERY parallelFor call; with ~60 calls per step
    // (the pressure solve alone is dozens) that thread-churn dominated and stopped the
    // solver from scaling with cell count. These threads are created once and parked on
    // a condition variable between dispatches; work is claimed via an atomic ticket so
    // load stays balanced even when chunks finish unevenly.
    std::vector<std::thread> poolThreads_;
    int poolN_ = 0;                              // worker thread count (caller also helps)
    std::mutex poolM_;
    std::condition_variable poolStartCv_;
    std::mutex poolDoneM_;
    std::condition_variable poolDoneCv_;
    std::atomic<int> poolNext_{0};               // next chunk index to claim
    std::atomic<int> poolDone_{0};               // chunks completed
    int poolCount_ = 0, poolChunk_ = 0, poolParts_ = 0;
    std::uint64_t poolGen_ = 0;                   // bumped to release a new dispatch
    bool poolStop_ = false;
    std::function<void(int, int)> poolFn_;        // operates on a [start,end) sub-range

    // background solver
    std::thread worker_;
    std::atomic<bool> alive_{false};
    mutable std::mutex stepMtx_; // protects the live arrays during a step vs. mutators
    mutable std::mutex histMtx_; // protects the history deque + viewSnap_
    std::mutex ctrlMtx_;         // protects the control inputs
    bool ctrlRunning_ = false;
    WeatherParams ctrlParams_{};
    glm::vec3 ctrlSun_{0.0f, 1.0f, 0.0f};

    // Active array selectors: the snapshot the render thread is viewing, else live arrays.
    const std::vector<float>& aU() const { return viewSnap_ ? viewSnap_->u : u_; }
    const std::vector<float>& aV() const { return viewSnap_ ? viewSnap_->v : v_; }
    const std::vector<float>& aW() const { return viewSnap_ ? viewSnap_->w : w_; }
    const std::vector<float>& aTheta() const { return viewSnap_ ? viewSnap_->theta : theta_; }
    const std::vector<float>& aQv() const { return viewSnap_ ? viewSnap_->qv : qv_; }
    const std::vector<float>& aQc() const { return viewSnap_ ? viewSnap_->qc : qc_; }
    const std::vector<float>& aQr() const { return viewSnap_ ? viewSnap_->qr : qr_; }

    std::shared_ptr<Snapshot> makeSnapshot() const
    {
        auto s = std::make_shared<Snapshot>();
        s->u = u_; s->v = v_; s->w = w_; s->theta = theta_; s->qv = qv_; s->qc = qc_; s->qr = qr_;
        s->simTime = simTime_;
        return s;
    }

    void stepInternal(float realDt, const WeatherParams& p, const glm::vec3& sunDir)
    {
        vScale_ = p.vScale;
        float simDt = std::min(realDt * p.timeScale, 4.0f);
        int sub = std::max(1, static_cast<int>(std::ceil(simDt / 1.2f)));
        float dt = simDt / static_cast<float>(sub);
        for (int s = 0; s < sub; ++s) integrate(dt, p, sunDir);
        simTime_ += simDt;
    }

    void workerLoop()
    {
        using clock = std::chrono::steady_clock;
        auto last = clock::now();
        while (alive_.load()) {
            bool run; WeatherParams p; glm::vec3 sun;
            { std::lock_guard<std::mutex> lk(ctrlMtx_); run = ctrlRunning_; p = ctrlParams_; sun = ctrlSun_; }
            auto now = clock::now();
            float realDt = std::chrono::duration<float>(now - last).count();
            last = now;
            if (!run || !configured_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                last = clock::now();
                continue;
            }
            realDt = std::min(std::max(realDt, 0.0f), 0.05f);
            std::shared_ptr<const Snapshot> snap;
            {
                std::lock_guard<std::mutex> lk(stepMtx_);
                stepInternal(realDt, p, sun);
                snap = makeSnapshot();
            }
            {
                std::lock_guard<std::mutex> lk(histMtx_);
                history_.push_back(snap);
                while (static_cast<int>(history_.size()) > maxHistory_) history_.pop_front();
            }
        }
    }

    std::size_t cellCount() const { return static_cast<std::size_t>(nx_) * ny_ * nz_; }
    int idx(int i, int j, int k) const { return (k * ny_ + j) * nx_ + i; }
    float cellY(int j) const { return baseY_ + (j + 0.5f) * dy_; }
    float physAlt(float y) const { return (y - baseY_) * vScale_; }
    float thetaEnv(int j, const WeatherParams& p) const
    {
        return (p.surfaceTempC + 273.15f) + p.thetaLapse * physAlt(cellY(j));
    }
    float qvEnvLevel(int j, const WeatherParams& p) const
    {
        float y = cellY(j);
        return p.inflowRH * qSat(thetaEnv(j, p) * exner(y), pressureAt(y));
    }
    float pressureAt(float y) const { return kP0 * std::exp(-physAlt(y) / 8500.0f); }
    // Adiabatic cooling on lift lives here. Over the literal ~150 m box it is tiny, so the
    // physics altitude is scaled (vScale_) to a realistic mountain depth; this is the entire
    // trigger for orographic condensation.
    float exner(float y) const { return std::pow(pressureAt(y) / kP0, kRd / kCp); }
    static float qSat(float T, float pres)
    {
        float Tc = T - 273.15f;
        float es = 611.2f * std::exp(17.67f * Tc / (Tc + 243.5f));
        es = std::min(es, pres * 0.98f);
        return 0.622f * es / std::max(pres - es, 1.0f);
    }
    glm::vec3 windVector(const WeatherParams& p) const
    {
        float a = glm::radians(p.windDirDeg);
        return {std::cos(a) * p.windSpeed, 0.0f, std::sin(a) * p.windSpeed};
    }

    template <class F>
    void parallelFor(int count, F fn) const
    {
        if (count <= 0) return;
        // Small loops or no pool: run inline (thread hand-off would cost more than it saves).
        if (count < 8192 || poolN_ <= 0) { for (int i = 0; i < count; ++i) fn(i); return; }

        auto* self = const_cast<Weather*>(this);
        // Slice into ~4 chunks per participant so the atomic-ticket scheduler can rebalance
        // when some rows hit solid cells early and finish faster than others.
        int parts = (poolN_ + 1) * 4;
        int chunk = (count + parts - 1) / parts;
        parts = (count + chunk - 1) / chunk; // trim trailing empty chunks
        {
            std::lock_guard<std::mutex> lk(self->poolM_);
            self->poolFn_ = [&fn](int s, int e) { for (int i = s; i < e; ++i) fn(i); };
            self->poolCount_ = count;
            self->poolChunk_ = chunk;
            self->poolParts_ = parts;
            self->poolNext_.store(0, std::memory_order_relaxed);
            self->poolDone_.store(0, std::memory_order_relaxed);
            ++self->poolGen_;
        }
        self->poolStartCv_.notify_all();
        self->poolDrain();      // caller participates as a worker
        // Wait until every chunk has been executed.
        std::unique_lock<std::mutex> lk(self->poolDoneM_);
        self->poolDoneCv_.wait(lk, [self] { return self->poolDone_.load(std::memory_order_acquire) >= self->poolParts_; });
        self->poolFn_ = nullptr;
    }

    // Claim and run chunks until none remain. Shared by the caller and every pool thread.
    void poolDrain()
    {
        for (;;) {
            int part = poolNext_.fetch_add(1, std::memory_order_relaxed);
            if (part >= poolParts_) break;
            int s = part * poolChunk_;
            int e = std::min(poolCount_, s + poolChunk_);
            if (s < e) poolFn_(s, e);
            if (poolDone_.fetch_add(1, std::memory_order_acq_rel) + 1 >= poolParts_) {
                std::lock_guard<std::mutex> lk(poolDoneM_);
                poolDoneCv_.notify_one();
            }
        }
    }

    void startPool()
    {
        if (poolN_ > 0) return;
        unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        poolN_ = std::min<int>(static_cast<int>(hw), 10) - 1; // caller is one participant
        if (poolN_ < 1) { poolN_ = 0; return; }
        poolStop_ = false;
        for (int t = 0; t < poolN_; ++t) {
            poolThreads_.emplace_back([this] {
                std::uint64_t seen = 0;
                for (;;) {
                    std::unique_lock<std::mutex> lk(poolM_);
                    poolStartCv_.wait(lk, [this, &seen] { return poolStop_ || poolGen_ != seen; });
                    if (poolStop_) return;
                    seen = poolGen_;
                    lk.unlock();
                    poolDrain();
                }
            });
        }
    }

    void stopPool()
    {
        if (poolN_ <= 0) return;
        { std::lock_guard<std::mutex> lk(poolM_); poolStop_ = true; ++poolGen_; }
        poolStartCv_.notify_all();
        for (auto& th : poolThreads_) if (th.joinable()) th.join();
        poolThreads_.clear();
        poolN_ = 0;
    }

    void rebuildMask()
    {
        for (int k = 0; k < nz_; ++k)
            for (int i = 0; i < nx_; ++i) {
                float h = colHeight_[static_cast<std::size_t>(k) * nx_ + i];
                int surf = ny_ - 1;
                for (int j = 0; j < ny_; ++j) {
                    bool s = cellY(j) < h;
                    solid_[idx(i, j, k)] = s ? 1u : 0u;
                    if (!s) { surf = j; break; }
                }
                surfaceJ_[static_cast<std::size_t>(k) * nx_ + i] = surf;
            }
        poissonDirty_ = true; // solid mask changed -> Poisson stencil must be rebuilt
    }

    // Build the per-cell Poisson stencil + red/black colour lists from the current solid
    // mask. Excluded neighbours (out of bounds or solid, i.e. Neumann walls) point at the
    // zero sentinel slot p_[cellCount()] so the sweep can gather all six branchlessly.
    void buildPoisson()
    {
        const int N = static_cast<int>(cellCount());
        const int Z = N; // sentinel zero slot in p_
        const float cx = 1.0f / (dx_ * dx_);
        const float cy = 1.0f / (dy_ * dy_);
        const float cz = 1.0f / (dz_ * dz_);
        nbXm_.assign(N, Z); nbXp_.assign(N, Z);
        nbYm_.assign(N, Z); nbYp_.assign(N, Z);
        nbZm_.assign(N, Z); nbZp_.assign(N, Z);
        invDen_.assign(N, 0.0f);
        redCells_.clear(); blackCells_.clear();
        for (int c = 0; c < N; ++c) {
            if (solid_[c]) continue;
            int i = c % nx_, j = (c / nx_) % ny_, k = c / (nx_ * ny_);
            float den = 0.0f;
            auto set = [&](bool inb, int nb, float coef, int& slot) {
                if (inb && !solid_[nb]) { slot = nb; den += coef; }
            };
            set(i + 1 < nx_, idx(i + 1, j, k), cx, nbXp_[c]);
            set(i - 1 >= 0, idx(i - 1, j, k), cx, nbXm_[c]);
            set(j + 1 < ny_, idx(i, j + 1, k), cy, nbYp_[c]);
            set(j - 1 >= 0, idx(i, j - 1, k), cy, nbYm_[c]);
            set(k + 1 < nz_, idx(i, j, k + 1), cz, nbZp_[c]);
            set(k - 1 >= 0, idx(i, j, k - 1), cz, nbZm_[c]);
            if (den <= 0.0f) continue; // isolated cell: pressure stays 0
            invDen_[c] = 1.0f / den;
            if ((i + j + k) & 1) blackCells_.push_back(c);
            else redCells_.push_back(c);
        }
        poissonDirty_ = false;
    }

    // --- field sampling for advection (trilinear, clamped) ---
    float trilinear(const std::vector<float>& f, float gi, float gj, float gk) const
    {
        gi = glm::clamp(gi, 0.0f, nx_ - 1.001f);
        gj = glm::clamp(gj, 0.0f, ny_ - 1.001f);
        gk = glm::clamp(gk, 0.0f, nz_ - 1.001f);
        int i0 = static_cast<int>(gi), j0 = static_cast<int>(gj), k0 = static_cast<int>(gk);
        int i1 = std::min(i0 + 1, nx_ - 1), j1 = std::min(j0 + 1, ny_ - 1), k1 = std::min(k0 + 1, nz_ - 1);
        float fi = gi - i0, fj = gj - j0, fk = gk - k0;
        auto L = [&](int kk, int jj) {
            float a = f[idx(i0, jj, kk)], b = f[idx(i1, jj, kk)];
            return a + (b - a) * fi;
        };
        auto P = [&](int kk) {
            float a = L(kk, j0), b = L(kk, j1);
            return a + (b - a) * fj;
        };
        float a = P(k0), b = P(k1);
        return a + (b - a) * fk;
    }

    void semiLagrangian(const std::vector<float>& src, std::vector<float>& dst, float dt, float extraVy) const
    {
        parallelFor(static_cast<int>(cellCount()), [&](int c) {
            if (solid_[c]) { dst[c] = src[c]; return; }
            int i = c % nx_;
            int j = (c / nx_) % ny_;
            int k = c / (nx_ * ny_);
            float gi = i - (u_[c] * dt) / dx_;
            float gj = j - ((v_[c] + extraVy) * dt) / dy_;
            float gk = k - (w_[c] * dt) / dz_;
            dst[c] = trilinear(src, gi, gj, gk);
        });
    }

    float vorticityMag(int i, int j, int k) const
    {
        int ip = std::min(i + 1, nx_ - 1), im = std::max(i - 1, 0);
        int jp = std::min(j + 1, ny_ - 1), jm = std::max(j - 1, 0);
        int kp = std::min(k + 1, nz_ - 1), km = std::max(k - 1, 0);
        const auto& U = aU(); const auto& V = aV(); const auto& W = aW();
        float dwdy = (W[idx(i, jp, k)] - W[idx(i, jm, k)]) / (2 * dy_);
        float dvdz = (V[idx(i, j, kp)] - V[idx(i, j, km)]) / (2 * dz_);
        float dudz = (U[idx(i, j, kp)] - U[idx(i, j, km)]) / (2 * dz_);
        float dwdx = (W[idx(ip, j, k)] - W[idx(im, j, k)]) / (2 * dx_);
        float dvdx = (V[idx(ip, j, k)] - V[idx(im, j, k)]) / (2 * dx_);
        float dudy = (U[idx(i, jp, k)] - U[idx(i, jm, k)]) / (2 * dy_);
        glm::vec3 curl(dwdy - dvdz, dudz - dwdx, dvdx - dudy);
        return glm::length(curl);
    }

    void applyInflow(const WeatherParams& p)
    {
        glm::vec3 wind = windVector(p);
        bool xIn0 = wind.x >= 0.0f;   // inflow on i=0 if wind blows toward +x
        bool zIn0 = wind.z >= 0.0f;
        auto setInflowCol = [&](int i, int k) {
            for (int j = 0; j < ny_; ++j) {
                int c = idx(i, j, k);
                if (solid_[c]) continue;
                u_[c] = wind.x; v_[c] = 0.0f; w_[c] = wind.z;
                float th = thetaEnv(j, p);
                theta_[c] = th;
                float T = th * exner(cellY(j));
                qv_[c] = p.inflowRH * qSat(T, pressureAt(cellY(j)));
                qc_[c] = 0.0f; qr_[c] = 0.0f;
            }
        };
        int xi = xIn0 ? 0 : nx_ - 1;
        int zk = zIn0 ? 0 : nz_ - 1;
        if (std::abs(wind.x) >= std::abs(wind.z)) {
            for (int k = 0; k < nz_; ++k) setInflowCol(xi, k);
        } else {
            for (int i = 0; i < nx_; ++i) setInflowCol(i, zk);
        }
    }

    void zeroGradientOutflow(const WeatherParams& p)
    {
        glm::vec3 wind = windVector(p);
        auto copyCol = [&](int dstI, int dstK, int srcI, int srcK) {
            for (int j = 0; j < ny_; ++j) {
                int d = idx(dstI, j, dstK), s = idx(srcI, j, srcK);
                if (solid_[d]) continue;
                u_[d] = u_[s]; v_[d] = v_[s]; w_[d] = w_[s];
                theta_[d] = theta_[s]; qv_[d] = qv_[s]; qc_[d] = qc_[s]; qr_[d] = qr_[s];
            }
        };
        if (std::abs(wind.x) >= std::abs(wind.z)) {
            int o = (wind.x >= 0.0f) ? nx_ - 1 : 0;
            int s = (wind.x >= 0.0f) ? nx_ - 2 : 1;
            for (int k = 0; k < nz_; ++k) copyCol(o, k, s, k);
        } else {
            int o = (wind.z >= 0.0f) ? nz_ - 1 : 0;
            int s = (wind.z >= 0.0f) ? nz_ - 2 : 1;
            for (int i = 0; i < nx_; ++i) copyCol(i, o, i, s);
        }
    }

    void integrate(float dt, const WeatherParams& p, const glm::vec3& sunDir)
    {
        glm::vec3 wind = windVector(p);

        // 1. Body forces: buoyancy on vertical velocity + interior wind relaxation + drag.
        parallelFor(static_cast<int>(cellCount()), [&](int c) {
            if (solid_[c]) { u_[c] = v_[c] = w_[c] = 0.0f; return; }
            int j = (c / nx_) % ny_;
            float th0 = thetaEnv(j, p);
            float qv0 = qvEnvLevel(j, p);
            // Bound the potential-temperature anomaly. Real convective plumes stay within a
            // few K of the environment; a wider band here is only a safety rail that stops an
            // explicit latent-heating overshoot (possible after an unusually large substep)
            // from running theta away and seeding NaNs through the buoyancy/thermo chain.
            theta_[c] = glm::clamp(theta_[c], th0 - 30.0f, th0 + 30.0f);
            float buoy = kG * ((theta_[c] - th0) / th0 + 0.61f * (qv_[c] - qv0) - qc_[c] - qr_[c]);
            v_[c] += p.buoyancy * buoy * dt;
            // sponge near lid to damp vertical reflections (damping fraction clamped to [0,1]
            // so a large substep can never flip the sign and amplify the top cells)
            float topFrac = static_cast<float>(j) / static_cast<float>(ny_ - 1);
            if (topFrac > 0.82f) {
                float s = (topFrac - 0.82f) / 0.18f;
                float damp = std::min(1.0f, 3.0f * s * dt);
                v_[c] *= (1.0f - damp);
                // Rayleigh-relax thermo/moisture back toward the environment so heat,
                // vapor and condensate cannot accumulate against the rigid lid (that
                // pile-up was producing a band of bogus cloud + extreme values at the
                // very top that blew out the color range over the rest of the field).
                float sdamp = std::min(1.0f, 2.0f * s * dt);
                theta_[c] += (th0 - theta_[c]) * sdamp;
                qv_[c]    += (qv0 - qv_[c]) * sdamp;
                qc_[c]    *= (1.0f - sdamp);
                qr_[c]    *= (1.0f - sdamp);
            }
            // relax horizontal wind toward prevailing (large-scale pressure gradient proxy)
            u_[c] += (wind.x - u_[c]) * std::min(1.0f, p.windNudge * dt);
            w_[c] += (wind.z - w_[c]) * std::min(1.0f, p.windNudge * dt);
            // safety clamp so a transient can never run the solver away
            u_[c] = glm::clamp(u_[c], -35.0f, 35.0f);
            v_[c] = glm::clamp(v_[c], -35.0f, 35.0f);
            w_[c] = glm::clamp(w_[c], -35.0f, 35.0f);
        });

        // 2. Surface energy: shadowed solar heating / IR cooling -> drives slope winds.
        surfaceForcing(dt, p, sunDir);

        // 3. Advect velocity (semi-Lagrangian).
        semiLagrangian(u_, u2_, dt, 0.0f);
        semiLagrangian(v_, v2_, dt, 0.0f);
        semiLagrangian(w_, w2_, dt, 0.0f);
        u_.swap(u2_); v_.swap(v2_); w_.swap(w2_);

        // 4. Advect scalars.
        semiLagrangian(theta_, s2_, dt, 0.0f); theta_.swap(s2_);
        semiLagrangian(qv_, s2_, dt, 0.0f); qv_.swap(s2_);
        semiLagrangian(qc_, s2_, dt, 0.0f); qc_.swap(s2_);
        semiLagrangian(qr_, s2_, dt, -p.rainFall); qr_.swap(s2_); // rain falls

        // 5. Boundaries before projection.
        applyInflow(p);

        // 6. Microphysics (condensation/evaporation/autoconversion/rain evap) + precip.
        microphysics(dt, p);

        // 7. Pressure projection -> divergence free.
        project(dt, p);

        // 8. Rigid lid: no flow through the top boundary (kills top-corner artifacts).
        parallelFor(nx_ * nz_, [&](int col) {
            int i = col % nx_, k = col / nx_;
            int c = idx(i, ny_ - 1, k);
            if (!solid_[c]) v_[c] = 0.0f;
        });

        // 9. Sub-grid mixing (poor-man's Smagorinsky) damps grid-scale noise.
        diffuse(u_, 0.04f); diffuse(v_, 0.04f); diffuse(w_, 0.04f);
        diffuse(theta_, 0.02f); diffuse(qv_, 0.02f);

        // 9. Outflow.
        zeroGradientOutflow(p);

        // 10. Finalize: the pressure correction (step 7) and diffusion (step 8) write the
        // velocities that get published without any further clamp, so guarantee here that
        // every field handed to the renderer is finite and physically bounded. A blowup can
        // then at worst be a damped local transient -- never an all-NaN frame.
        parallelFor(static_cast<int>(cellCount()), [&](int c) {
            if (solid_[c]) return;
            auto scrub = [](float& x, float lo, float hi, float fallback) {
                if (!std::isfinite(x)) x = fallback;
                else x = glm::clamp(x, lo, hi);
            };
            int j = (c / nx_) % ny_;
            float th0 = thetaEnv(j, p);
            scrub(u_[c], -45.0f, 45.0f, 0.0f);
            scrub(v_[c], -45.0f, 45.0f, 0.0f);
            scrub(w_[c], -45.0f, 45.0f, 0.0f);
            scrub(theta_[c], th0 - 30.0f, th0 + 30.0f, th0);
            scrub(qv_[c], 0.0f, 0.05f, 0.0f);
            scrub(qc_[c], 0.0f, 0.05f, 0.0f);
            scrub(qr_[c], 0.0f, 0.05f, 0.0f);
        });
    }

    void diffuse(std::vector<float>& f, float alpha)
    {
        parallelFor(static_cast<int>(cellCount()), [&](int c) {
            if (solid_[c]) { s2_[c] = f[c]; return; }
            int i = c % nx_, j = (c / nx_) % ny_, k = c / (nx_ * ny_);
            float sum = 0.0f; int n = 0;
            auto add = [&](bool ok, int nb) { if (ok && !solid_[nb]) { sum += f[nb]; ++n; } };
            add(i + 1 < nx_, idx(i + 1, j, k)); add(i - 1 >= 0, idx(i - 1, j, k));
            add(j + 1 < ny_, idx(i, j + 1, k)); add(j - 1 >= 0, idx(i, j - 1, k));
            add(k + 1 < nz_, idx(i, j, k + 1)); add(k - 1 >= 0, idx(i, j, k - 1));
            s2_[c] = (n > 0) ? f[c] + alpha * (sum / n - f[c]) : f[c];
        });
        f.swap(s2_);
    }

    void surfaceForcing(float dt, const WeatherParams& p, const glm::vec3& sunDir)
    {
        float horiz = std::sqrt(sunDir.x * sunDir.x + sunDir.z * sunDir.z) + 1e-4f;
        bool daytime = sunDir.y > 0.02f;
        parallelFor(static_cast<int>(static_cast<std::size_t>(nx_) * nz_), [&](int col) {
            int i = col % nx_;
            int k = col / nx_;
            int j = surfaceJ_[col];
            int c = idx(i, j, k);
            if (solid_[c]) return;
            // surface normal from column-height gradient
            int im = std::max(i - 1, 0), ip = std::min(i + 1, nx_ - 1);
            int km = std::max(k - 1, 0), kp = std::min(k + 1, nz_ - 1);
            float hx = (colHeight_[static_cast<std::size_t>(k) * nx_ + ip] - colHeight_[static_cast<std::size_t>(k) * nx_ + im]) / (2 * dx_);
            float hz = (colHeight_[static_cast<std::size_t>(kp) * nx_ + i] - colHeight_[static_cast<std::size_t>(km) * nx_ + i]) / (2 * dz_);
            glm::vec3 normal = glm::normalize(glm::vec3(-hx, 1.0f, -hz));

            float solar = 0.0f;
            if (daytime) {
                float cosInc = std::max(0.0f, glm::dot(normal, sunDir));
                float shade = shadowFactor(i, k, sunDir, horiz);
                solar = p.solarHeating * cosInc * shade;
            }
            float ir = p.irCooling; // longwave loss, stronger at night when solar is gone
            theta_[c] += (solar - ir) * dt;
            // saturate the surface-air temperature anomaly so heating can't accumulate without bound
            float th0 = thetaEnv(j, p);
            theta_[c] = glm::clamp(theta_[c], th0 - 6.0f, th0 + 6.0f);
            // surface friction
            float drag = std::min(1.0f, p.surfaceDrag * dt);
            u_[c] *= (1.0f - drag); w_[c] *= (1.0f - drag);
        });
    }

    float shadowFactor(int i, int k, const glm::vec3& sunDir, float horiz) const
    {
        // March toward the sun across columns; if terrain rises above the ray, we're shaded.
        float h0 = colHeight_[static_cast<std::size_t>(k) * nx_ + i] + 0.5f;
        float stepX = sunDir.x / horiz; // unit horizontal direction toward sun
        float stepZ = sunDir.z / horiz;
        float slope = sunDir.y / horiz; // vertical rise per unit horizontal distance
        float fx = static_cast<float>(i);
        float fk = static_cast<float>(k);
        float dist = 0.0f;
        float stepLen = 1.0f; // in cells
        for (int s = 0; s < 48; ++s) {
            fx += stepX * stepLen;
            fk += stepZ * stepLen;
            dist += stepLen * std::min(dx_, dz_);
            int ci = static_cast<int>(std::lround(fx));
            int ck = static_cast<int>(std::lround(fk));
            if (ci < 0 || ci >= nx_ || ck < 0 || ck >= nz_) break;
            float rayY = h0 + slope * dist;
            if (rayY > lidY_) break;
            float terr = colHeight_[static_cast<std::size_t>(ck) * nx_ + ci];
            if (terr > rayY) return 0.18f; // ambient/diffuse floor when shadowed
        }
        return 1.0f;
    }

    void microphysics(float dt, const WeatherParams& p)
    {
        parallelFor(static_cast<int>(cellCount()), [&](int c) {
            if (solid_[c]) return;
            int j = (c / nx_) % ny_;
            float y = cellY(j);
            float pres = pressureAt(y);
            float ex = exner(y);
            float T = theta_[c] * ex;
            float qs = qSat(T, pres);

            // saturation adjustment (condensation with latent heating)
            if (qv_[c] > qs) {
                float denom = 1.0f + (kL * kL * qs) / (kCp * 461.0f * T * T);
                float cond = (qv_[c] - qs) / denom;
                cond = std::min(cond, qv_[c]);
                qv_[c] -= cond; qc_[c] += cond;
                theta_[c] += (kL / kCp) * cond / ex;
            } else if (qc_[c] > 0.0f) {
                float denom = 1.0f + (kL * kL * qs) / (kCp * 461.0f * T * T);
                float evap = std::min(qc_[c], (qs - qv_[c]) / denom);
                qv_[c] += evap; qc_[c] -= evap;
                theta_[c] -= (kL / kCp) * evap / ex;
            }
            // autoconversion cloud -> rain
            if (qc_[c] > p.autoThresh) {
                float conv = p.autoconv * (qc_[c] - p.autoThresh) * dt;
                conv = std::min(conv, qc_[c]);
                qc_[c] -= conv; qr_[c] += conv;
            }
            // rain evaporation in subsaturated air
            if (qr_[c] > 0.0f && qv_[c] < qs) {
                float evap = p.rainEvap * (qs - qv_[c]) * dt;
                evap = std::min(evap, qr_[c]);
                qr_[c] -= evap; qv_[c] += evap;
                theta_[c] -= (kL / kCp) * evap / ex; // evaporative cooling -> downdrafts
            }
            qv_[c] = std::max(qv_[c], 0.0f);
            qc_[c] = std::max(qc_[c], 0.0f);
            qr_[c] = std::max(qr_[c], 0.0f);
        });

        // Precipitation reaching the surface: drain rain from each surface cell.
        float decay = std::pow(0.5f, dt / 30.0f); // ~30 s half-life for the live rate display
        for (int col = 0; col < nx_ * nz_; ++col) precipRate_[col] *= decay;
        parallelFor(nx_ * nz_, [&](int col) {
            int i = col % nx_;
            int k = col / nx_;
            int j = surfaceJ_[col];
            int c = idx(i, j, k);
            if (solid_[c]) return;
            float frac = std::min(1.0f, p.rainFall * dt / dy_);
            float fall = qr_[c] * frac;
            if (fall <= 0.0f) return;
            qr_[c] -= fall;
            precipRate_[col] += fall;
            precipAccum_[col] += fall;
        });
    }

    void project(float dt, const WeatherParams& p)
    {
        const float cx = 1.0f / (dx_ * dx_);
        const float cy = 1.0f / (dy_ * dy_);
        const float cz = 1.0f / (dz_ * dz_);

        // divergence
        parallelFor(static_cast<int>(cellCount()), [&](int c) {
            if (solid_[c]) { div_[c] = 0.0f; return; }
            int i = c % nx_, j = (c / nx_) % ny_, k = c / (nx_ * ny_);
            float uR = (i + 1 < nx_ && !solid_[idx(i + 1, j, k)]) ? u_[idx(i + 1, j, k)] : -u_[c];
            float uL = (i - 1 >= 0 && !solid_[idx(i - 1, j, k)]) ? u_[idx(i - 1, j, k)] : -u_[c];
            float vU = (j + 1 < ny_ && !solid_[idx(i, j + 1, k)]) ? v_[idx(i, j + 1, k)] : -v_[c];
            float vD = (j - 1 >= 0 && !solid_[idx(i, j - 1, k)]) ? v_[idx(i, j - 1, k)] : -v_[c];
            float wF = (k + 1 < nz_ && !solid_[idx(i, j, k + 1)]) ? w_[idx(i, j, k + 1)] : -w_[c];
            float wB = (k - 1 >= 0 && !solid_[idx(i, j, k - 1)]) ? w_[idx(i, j, k - 1)] : -w_[c];
            div_[c] = ((uR - uL) / (2 * dx_) + (vU - vD) / (2 * dy_) + (wF - wB) / (2 * dz_)) / dt;
        });

        // Red-black Gauss-Seidel with SOR. Updating one colour at a time is race-free under
        // parallelism (every neighbour is the opposite colour) and the over-relaxation factor
        // makes it converge several times faster than Jacobi, so high-resolution grids still
        // reach a near-divergence-free state within the same iteration budget. The stencil
        // (neighbour indices + inverse diagonal + colour lists) is precomputed per terrain
        // change, so each sweep is a pure gather over only the cells of one colour.
        if (poissonDirty_) buildPoisson();
        const float omega = 1.7f;
        std::fill(p_.begin(), p_.end(), 0.0f);
        const int* xm = nbXm_.data(); const int* xp = nbXp_.data();
        const int* ym = nbYm_.data(); const int* yp = nbYp_.data();
        const int* zm = nbZm_.data(); const int* zp = nbZp_.data();
        const float* idn = invDen_.data();
        const float* dv = div_.data();
        float* pp = p_.data();
        auto sweep = [&](const std::vector<int>& cells) {
            parallelFor(static_cast<int>(cells.size()), [&](int t) {
                int c = cells[t];
                float num = cx * (pp[xm[c]] + pp[xp[c]])
                          + cy * (pp[ym[c]] + pp[yp[c]])
                          + cz * (pp[zm[c]] + pp[zp[c]]);
                pp[c] += omega * ((num - dv[c]) * idn[c] - pp[c]);
            });
        };
        for (int it = 0; it < p.pressureIters; ++it) {
            sweep(redCells_);
            sweep(blackCells_);
        }

        // velocity correction
        parallelFor(static_cast<int>(cellCount()), [&](int c) {
            if (solid_[c]) { u_[c] = v_[c] = w_[c] = 0.0f; return; }
            int i = c % nx_, j = (c / nx_) % ny_, k = c / (nx_ * ny_);
            float pR = (i + 1 < nx_ && !solid_[idx(i + 1, j, k)]) ? p_[idx(i + 1, j, k)] : p_[c];
            float pL = (i - 1 >= 0 && !solid_[idx(i - 1, j, k)]) ? p_[idx(i - 1, j, k)] : p_[c];
            float pU = (j + 1 < ny_ && !solid_[idx(i, j + 1, k)]) ? p_[idx(i, j + 1, k)] : p_[c];
            float pD = (j - 1 >= 0 && !solid_[idx(i, j - 1, k)]) ? p_[idx(i, j - 1, k)] : p_[c];
            float pF = (k + 1 < nz_ && !solid_[idx(i, j, k + 1)]) ? p_[idx(i, j, k + 1)] : p_[c];
            float pB = (k - 1 >= 0 && !solid_[idx(i, j, k - 1)]) ? p_[idx(i, j, k - 1)] : p_[c];
            u_[c] -= dt * (pR - pL) / (2 * dx_);
            v_[c] -= dt * (pU - pD) / (2 * dy_);
            w_[c] -= dt * (pF - pB) / (2 * dz_);
        });
    }
};
