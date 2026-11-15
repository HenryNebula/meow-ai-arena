// Nova Salvo -- a from-scratch Bomberman bot for the meow-ai-arena community.
//
// Written from scratch for this submission; it uses only the public text
// protocol and the deterministic rules published in the episode RULES.
// No official entry, baseline or other submission was consulted.
//
// Strategy in one paragraph:
//   Every turn the bot mirrors the referee's deterministic phases (closing
//   walls, bomb chains, crate destruction, flame lifetimes) across a short
//   horizon. On top of that predicted timeline it runs a time-expanded
//   survival search (forward reachability plus a backward "alive at the
//   horizon" pass). An action is only considered safe if the bot can still
//   be alive at the horizon after taking it. Candidate bomb drops are
//   judged inside a parallel simulated world: opponents whose survival
//   search collapses there are counted as kills, with a flame-ownership
//   check for kill credit. A counterfactual world in which the nearest
//   armed rival drops a bomb right now guards against being trapped.
//   Long-range movement is steered by a persistent goal cell scored from
//   crate value, enemy coverage and how long cells stay open as the arena
//   closes. Survival always dominates: it is worth more than any kill.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// Protocol constants and small helpers.
// ---------------------------------------------------------------------------

static const int MAXN = 27;              // boards are at most 25x25
static const int MAXCELLS = MAXN * MAXN;
static const int HORIZON = 12;           // simulated turns per decision
static const int MAXP = 10;

static const int DX[5] = {0, 0, -1, 1, 0};
static const int DY[5] = {-1, 1, 0, 0, 0};
static const char DCH[5] = {'U', 'D', 'L', 'R', 'S'};

static int N = 0, P = 0, ME = 0, LIMIT = 0;
static int FUSE = 4, RANGE = 3, CAP = 2, FIRE = 2, SHRINK = 0, EVERY = 20;
static int N2 = 0;

static inline int C(int x, int y) { return y * N + x; }
static inline int CX(int c) { return c % N; }
static inline int CY(int c) { return c / N; }

struct BombH {
    int owner, x, y, at;
};

// ---------------------------------------------------------------------------
// Parsed state for the current turn.
// ---------------------------------------------------------------------------

static array<string, MAXN> board;        // rows of "# + ."
static int px[MAXP], py[MAXP];
static bool aliveP[MAXP];
static vector<BombH> bombs;              // all live bombs
static int flameEnd0[MAXCELLS];          // active flames at input, -1 = none
static int T = 0;

// ---------------------------------------------------------------------------
// Static board precomputation (done once at INIT).
// ---------------------------------------------------------------------------

static int cellD[MAXCELLS];              // distance from the outer boundary
static int closedAt[MAXCELLS];           // absolute turn the cell turns to wall

static void precomputeGeometry() {
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            int d = min(min(x, y), min(N - 1 - x, N - 1 - y));
            int c = C(x, y);
            cellD[c] = d;
            // Layer 1 closes at SHRINK, layer d at SHRINK + (d-1)*EVERY.
            closedAt[c] = (d <= 0) ? 0 : SHRINK + (d - 1) * EVERY;
        }
}

// ---------------------------------------------------------------------------
// Deterministic forward simulation of the referee phases, sans player moves.
//
// For every future turn T+k (k = 0..h-1) it produces:
//   entry[k][c] -- the cell may be ENTERED during that turn's movement phase
//                  (open ground, no crate, no bomb still standing).
//   burn[k][c]  -- the cell is lethal after that turn's movement phase
//                  (flames from explosions at T+k-1 or T+k).
//
// trackIdx optionally names one bomb of bombsIn whose own blast cells (the
// turns they are lethal) are recorded in flameMine, for kill attribution.
// ---------------------------------------------------------------------------

struct World {
    int h = 0;
    uint8_t entry[HORIZON + 2][MAXCELLS];
    uint8_t burn[HORIZON + 2][MAXCELLS];
    int cratesDestroyed = 0;
    int trackExplodeLayer = -1;          // layer the tracked bomb went off
};

static void simulate(const vector<BombH>& bombsIn, int h, World& w,
                     int trackIdx = -1,
                     uint8_t flameMine[HORIZON + 2][MAXCELLS] = nullptr) {
    w.h = h;
    w.cratesDestroyed = 0;
    w.trackExplodeLayer = -1;
    if (flameMine)
        memset(flameMine, 0, sizeof(uint8_t) * (size_t)(HORIZON + 2) * MAXCELLS);

    char g[MAXCELLS];
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) g[C(x, y)] = board[y][x];
    int flame[MAXCELLS];
    memcpy(flame, flameEnd0, sizeof(int) * N2);

    vector<BombH> bs = bombsIn;
    // Scratch buffers reused across layers.
    static int atCell[MAXCELLS];
    static uint8_t gone[64];
    static uint8_t hit[MAXCELLS];
    static uint8_t crateHit[MAXCELLS];
    static int queueArr[64];

    for (int k = 0; k < h; ++k) {
        int now = T + k;

        // Phase 1: expire old flames, then deterministic closing walls.
        for (int c = 0; c < N2; ++c)
            if (flame[c] < now) flame[c] = -1;
        int layer = (now < SHRINK) ? 0 : 1 + (now - SHRINK) / EVERY;
        if (now >= SHRINK && (now - SHRINK) % EVERY == 0) {
            for (int c = 0; c < N2; ++c)
                if (cellD[c] <= layer) { g[c] = '#'; flame[c] = -1; }
            size_t m = 0;
            for (size_t i = 0; i < bs.size(); ++i)
                if (g[C(bs[i].x, bs[i].y)] != '#') bs[m++] = bs[i];
            bs.resize(m);
        }

        // Phase 3 terrain snapshot: walls, crates and standing bombs block.
        for (int c = 0; c < N2; ++c) w.entry[k][c] = (g[c] == '.') ? 1 : 0;
        for (const BombH& b : bs) w.entry[k][C(b.x, b.y)] = 0;

        // Phase 4: explosions from one pre-explosion snapshot, chains same
        // turn. A hit bomb triggers immediately and stops its ray; a hit
        // crate burns and stops its ray; walls block without burning.
        int nb = (int)bs.size();
        memset(atCell, -1, sizeof(int) * N2);
        for (int i = 0; i < nb; ++i) atCell[C(bs[i].x, bs[i].y)] = i;
        memset(hit, 0, N2);
        memset(crateHit, 0, N2);
        memset(gone, 0, sizeof(uint8_t) * (size_t)(nb + 1));
        int qh = 0, qt = 0;
        for (int i = 0; i < nb; ++i)
            if (bs[i].at <= now || flame[C(bs[i].x, bs[i].y)] >= now)
                queueArr[qt++] = i;
        while (qh < qt) {
            int i = queueArr[qh++];
            if (gone[i]) continue;
            gone[i] = 1;
            int bc = C(bs[i].x, bs[i].y);
            hit[bc] = 1;
            if (i == trackIdx) {
                w.trackExplodeLayer = k;
                if (flameMine) {
                    flameMine[k][bc] = 1;
                    if (k + 1 <= h) flameMine[k + 1][bc] = 1;
                }
            }
            for (int d = 0; d < 4; ++d) {
                for (int r = 1; r <= RANGE; ++r) {
                    int x = bs[i].x + DX[d] * r, y = bs[i].y + DY[d] * r;
                    if (x < 0 || y < 0 || x >= N || y >= N || g[C(x, y)] == '#') break;
                    int c = C(x, y);
                    hit[c] = 1;
                    if (i == trackIdx && flameMine) {
                        // Crates and bombs burn too; they merely stop the ray.
                        flameMine[k][c] = 1;
                        if (k + 1 <= h) flameMine[k + 1][c] = 1;
                    }
                    if (g[c] == '+') { crateHit[c] = 1; break; }
                    if (atCell[c] >= 0) {
                        if (!gone[atCell[c]] && qt < 64) queueArr[qt++] = atCell[c];
                        break;
                    }
                }
            }
        }
        size_t m = 0;
        for (int i = 0; i < nb; ++i)
            if (!gone[i]) bs[m++] = bs[i];
        bs.resize(m);
        for (int c = 0; c < N2; ++c) {
            if (crateHit[c]) { g[c] = '.'; ++w.cratesDestroyed; }
            if (hit[c]) flame[c] = max(flame[c], now + FIRE - 1);
        }

        // Phase 5 lethality: flames from last turn (end == now) and this one.
        for (int c = 0; c < N2; ++c) w.burn[k][c] = (flame[c] >= now) ? 1 : 0;
    }
}

// ---------------------------------------------------------------------------
// Time-expanded survival search over a simulated world.
//
// State (k, c): alive at the input of turn T+k standing on c, with that
// turn's closing walls survived. Transitions pick a move for turn T+k:
//   * waiting is always allowed (one may stand on one's own bomb);
//   * stepping requires entry[k][target];
//   * the destination must not burn at T+k (phase 5 check);
//   * the destination must survive the next turn's closing walls.
// R[k] is the forward-reachable set; B[k] is the subset from which the
// horizon layer h is still reachable (backward pass). B[1] therefore marks
// the destinations that keep the agent alive for the whole horizon.
// ---------------------------------------------------------------------------

struct Reach {
    uint8_t R[HORIZON + 2][MAXCELLS];
    uint8_t B[HORIZON + 2][MAXCELLS];
    int lastLayer = -1;                  // highest k with R[k] non-empty
    int horizonAlive = 0;                // |R[h]|, freedom at the horizon
};

// block0 (optional): cells occupied by other agents' bodies right now. They
// may stay put, so entering their cell this turn is not guaranteed; the
// search treats it as blocked (conservative but sound).
// shadow (optional): per-cell distance to the nearest rival body. A rival
// moves one cell per turn, so a cell within distance k+1 of a rival body may
// be standing on it (or bombing it) when we arrive at layer k+1; such moves
// are treated as blocked. Waiting is never shadow-blocked: a rival cannot
// displace a player that stays put.
// pessBurn (optional): per-layer overlay of phantom flames -- cells an armed
// rival could have bombed by that layer (see buildPessBurn). Combined with
// shadow this makes the search answer "alive even under rival interference".
static void reachSearch(int sx, int sy, const World& w, int h, Reach& r,
                        const uint8_t* block0 = nullptr,
                        const int* shadow = nullptr,
                        const uint8_t (*pessBurn)[MAXCELLS] = nullptr,
                        bool bodyLinger = false) {
    memset(r.R, 0, sizeof(uint8_t) * (size_t)(HORIZON + 2) * MAXCELLS);
    memset(r.B, 0, sizeof(uint8_t) * (size_t)(HORIZON + 2) * MAXCELLS);
    r.lastLayer = -1;
    r.horizonAlive = 0;
    int c0 = C(sx, sy);
    if (closedAt[c0] <= T) return;       // closing walls take us this turn
    r.R[0][c0] = 1;
    r.lastLayer = 0;
    for (int k = 0; k < h; ++k) {
        int now = T + k;
        for (int c = 0; c < N2; ++c) {
            if (!r.R[k][c]) continue;
            int x = CX(c), y = CY(c);
            for (int d = 0; d < 5; ++d) {
                int nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                int nc = C(nx, ny);
                if (d < 4 && !w.entry[k][nc]) continue;   // terrain/bomb block
                if (d < 4 && k <= (bodyLinger ? 1 : 0) && block0 && block0[nc]) continue;
                if (d < 4 && shadow && shadow[nc] <= k + 1) continue;
                if (w.burn[k][nc]) continue;              // phase 5 flames
                if (pessBurn && pessBurn[k][nc]) continue;
                if (closedAt[nc] <= now + 1) continue;    // next closing walls
                r.R[k + 1][nc] = 1;
                if (k + 1 > r.lastLayer) r.lastLayer = k + 1;
            }
        }
    }
    if (r.lastLayer < h) return;         // doomed inside the horizon
    memcpy(r.B[h], r.R[h], N2);
    for (int k = h - 1; k >= 0; --k) {
        int now = T + k;
        for (int c = 0; c < N2; ++c) {
            if (!r.R[k][c]) continue;
            int x = CX(c), y = CY(c);
            for (int d = 0; d < 5; ++d) {
                int nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                int nc = C(nx, ny);
                if (d < 4 && !w.entry[k][nc]) continue;
                if (d < 4 && k <= (bodyLinger ? 1 : 0) && block0 && block0[nc]) continue;
                if (d < 4 && shadow && shadow[nc] <= k + 1) continue;
                if (w.burn[k][nc]) continue;
                if (pessBurn && pessBurn[k][nc]) continue;
                if (closedAt[nc] <= now + 1) continue;
                if (r.B[k + 1][nc]) { r.B[k][c] = 1; break; }
            }
        }
    }
    for (int c = 0; c < N2; ++c) r.horizonAlive += r.R[h][c];
}

// Phantom-flame overlay for the pessimistic tier: an armed rival reaching a
// cell after s turns can bomb it there, so the cell and its (wall-blocked)
// rays burn during layers s+FUSE and s+FUSE+1. Uses the CURRENT board for
// ray blocking; painting through crates is intentionally conservative.
static uint8_t pessBurn[HORIZON + 2][MAXCELLS];

static void buildPessBurn(const int* armedDist, int h) {
    memset(pessBurn, 0, sizeof(uint8_t) * (size_t)(HORIZON + 2) * MAXCELLS);
    for (int c = 0; c < N2; ++c) {
        int s = armedDist[c];
        if (s > h - FUSE - 1) continue;  // cannot explode inside the horizon
        for (int j = 0; j < 2; ++j) {
            int k = s + FUSE + j;
            if (k > h) break;
            pessBurn[k][c] = 1;
            for (int d = 0; d < 4; ++d)
                for (int r = 1; r <= RANGE; ++r) {
                    int nx = CX(c) + DX[d] * r, ny = CY(c) + DY[d] * r;
                    if (nx < 0 || ny < 0 || nx >= N || ny >= N) break;
                    if (board[ny][nx] == '#') break;
                    pessBurn[k][C(nx, ny)] = 1;
                }
        }
    }
}

// Maximum horizon layer still reachable when starting at layer k0 on c0.
// Only used for doomed situations, to buy the most remaining turns.
static int forwardDepth(const World& w, int k0, int c0, int h) {
    static uint8_t cur[MAXCELLS], nxt[MAXCELLS];
    memset(cur, 0, N2);
    cur[c0] = 1;
    int best = k0;
    for (int k = k0; k < h; ++k) {
        int now = T + k;
        memset(nxt, 0, N2);
        bool any = false;
        for (int c = 0; c < N2; ++c) {
            if (!cur[c]) continue;
            int x = CX(c), y = CY(c);
            for (int d = 0; d < 5; ++d) {
                int nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                int nc = C(nx, ny);
                if (d < 4 && !w.entry[k][nc]) continue;
                if (w.burn[k][nc]) continue;
                if (closedAt[nc] <= now + 1) continue;
                if (!nxt[nc]) { nxt[nc] = 1; any = true; }
            }
        }
        memcpy(cur, nxt, N2);
        if (any) best = k + 1; else break;
    }
    return best;
}

// Short-horizon maneuvering room: how many distinct cells remain visitable
// within `layers` turns of standing on c0 at layer k0, under the world's
// fuse/flame timeline. Dead-end nooks score 1-2, open ground scores high;
// used to keep away from pockets whose single exit a rival can plug.
static int wiggleRoom(const World& w, int k0, int c0, int layers) {
    static uint8_t seen[5][MAXCELLS], counted[MAXCELLS];
    int L = min(layers, 4);
    if (k0 + L > w.h) L = w.h - k0;
    if (L < 0) L = 0;
    memset(seen, 0, sizeof(uint8_t) * 5 * MAXCELLS);
    memset(counted, 0, N2);
    seen[0][c0] = 1;
    for (int k = 0; k < L; ++k) {
        int now = T + k0 + k;
        for (int c = 0; c < N2; ++c) {
            if (!seen[k][c]) continue;
            int x = CX(c), y = CY(c);
            for (int d = 0; d < 5; ++d) {
                int nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                int nc = C(nx, ny);
                if (d < 4 && !w.entry[k0 + k][nc]) continue;
                if (w.burn[k0 + k][nc]) continue;
                if (closedAt[nc] <= now + 1) continue;
                seen[k + 1][nc] = 1;
            }
        }
    }
    int cnt = 0;
    for (int k = 1; k <= L; ++k)
        for (int c = 0; c < N2; ++c)
            if (seen[k][c] && !counted[c]) { counted[c] = 1; ++cnt; }
    return cnt;
}

// ---------------------------------------------------------------------------
// Steering: a persistent goal cell scored from crate value, enemy coverage
// and remaining cell lifetime as the arena closes.
// ---------------------------------------------------------------------------

static int goalCell = -1;
static int distGoal[MAXCELLS];
static int recentCells[8];
static int recentLen = 0;

static void bfsFrom(int c0, const uint8_t walk[MAXCELLS], int out[MAXCELLS]) {
    for (int i = 0; i < N2; ++i) out[i] = -1;
    out[c0] = 0;                          // the start cell is always usable
    static int q[MAXCELLS];
    int qh = 0, qt = 0;
    q[qt++] = c0;
    while (qh < qt) {
        int c = q[qh++];
        int x = CX(c), y = CY(c);
        for (int d = 0; d < 4; ++d) {
            int nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
            int nc = C(nx, ny);
            if (out[nc] >= 0 || !walk[nc]) continue;
            out[nc] = out[c] + 1;
            q[qt++] = nc;
        }
    }
}

static int blastCrates(int x, int y) {
    int n = 0;
    for (int d = 0; d < 4; ++d)
        for (int r = 1; r <= RANGE; ++r) {
            int nx = x + DX[d] * r, ny = y + DY[d] * r;
            if (nx < 0 || ny < 0 || nx >= N || ny >= N) break;
            char ch = board[ny][nx];
            if (ch == '#') break;
            if (ch == '+') { ++n; break; }
        }
    return n;
}

static bool blockedBetween(int x0, int y0, int x1, int y1) {
    int dx = (x1 > x0) - (x1 < x0), dy = (y1 > y0) - (y1 < y0);
    int x = x0 + dx, y = y0 + dy;
    while (x != x1 || y != y1) {
        if (board[y][x] != '.') return true;
        for (const BombH& b : bombs)
            if (b.x == x && b.y == y) return true;
        x += dx; y += dy;
    }
    return false;
}

static bool alignedClear(int ax, int ay, int bx, int by) {
    if (ax == bx && abs(ay - by) >= 1 && abs(ay - by) <= RANGE)
        return !blockedBetween(ax, ay, bx, by);
    if (ay == by && abs(ax - bx) >= 1 && abs(ax - bx) <= RANGE)
        return !blockedBetween(ax, ay, bx, by);
    return false;
}

static int countBombsOf(int id) {
    int n = 0;
    for (const BombH& b : bombs)
        if (b.owner == id) ++n;
    return n;
}

static bool bombAt(int x, int y) {
    for (const BombH& b : bombs)
        if (b.x == x && b.y == y) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Per-turn decision.
// ---------------------------------------------------------------------------

static bool debugMode = false;           // stderr traces, local development only

static const double SAFE_W = 1.0e7;      // full-horizon survival secured
static const double DOOM_BASE = 2.0e6;   // baseline for doomed actions
static const double DOOM_STEP = 1.2e5;   // per extra turn bought
static const double KILL_W = 2.4e6;
static const double CAUT_B = 8.0e5;
static const double CAUT_P = 1.15e6;
static const double PESS_W = 4.0e5;      // survives even under rival shadow
static const double PESS_PLACE_LOST = 0.0;
static const double PESS_PLACE_RISK = 0.0;
static const double HOLD_W = 0.0;      // survives bodies lingering one turn
static const double HOLD_PLACE_LOST = 0.0;  // placing cancels that safety
static const double HOLD_PLACE_RISK = 0.0;  // placing with no such safety
static const double CRATE_W = 380.0;
static const double PRESSURE_W = 700.0;
static const double STEER_W = 620.0;
static const double WIGGLE_W = 600.0;
static const double PROX_W = 4000.0;   // tiebreak: armed-rival proximity

static void decide(int& outDir, int& outPlace) {
    auto t0 = chrono::steady_clock::now();
    auto ms = [&] {
        return chrono::duration_cast<chrono::milliseconds>(
                   chrono::steady_clock::now() - t0).count();
    };

    outDir = 4;
    outPlace = 0;

    int mx = px[ME], my = py[ME];
    int mc = C(mx, my);
    int h = LIMIT - T;
    if (h > HORIZON) h = HORIZON;
    if (h < 1) h = 1;

    int myActive = countBombsOf(ME);
    bool underBomb = bombAt(mx, my);
    bool canPlace = myActive < CAP && !underBomb;

    // --- Worlds and my own survival searches -------------------------------
    static World base, placeW, threatB, threatP;
    static uint8_t flameMine[HORIZON + 2][MAXCELLS];
    static Reach mine, mineP, mineT, mineTP, minePs, minePsP, mineH, mineHP;

    // Bodies of other agents block entry this turn (they may stay put).
    static uint8_t bodyBlock[MAXCELLS], bodyBlockE[MAXCELLS];
    memset(bodyBlock, 0, N2);
    for (int i = 0; i < P; ++i)
        if (i != ME && aliveP[i]) bodyBlock[C(px[i], py[i])] = 1;

    // Rival shadow: multi-source BFS distance to the nearest rival body.
    static int shadowDist[MAXCELLS], armedDist[MAXCELLS], shadowQ[MAXCELLS];
    for (int i = 0; i < N2; ++i) { shadowDist[i] = 999; armedDist[i] = 999; }
    {
        int qh = 0, qt = 0;
        for (int i = 0; i < P; ++i)
            if (i != ME && aliveP[i]) {
                int c = C(px[i], py[i]);
                if (shadowDist[c] > 0) { shadowDist[c] = 0; shadowQ[qt++] = c; }
            }
        while (qh < qt) {
            int c = shadowQ[qh++];
            int x = CX(c), y = CY(c);
            for (int d = 0; d < 4; ++d) {
                int nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                int nc = C(nx, ny);
                if (shadowDist[nc] > shadowDist[c] + 1) {
                    shadowDist[nc] = shadowDist[c] + 1;
                    shadowQ[qt++] = nc;
                }
            }
        }
        // Same BFS restricted to rivals that can still place a bomb.
        qh = 0; qt = 0;
        for (int i = 0; i < P; ++i)
            if (i != ME && aliveP[i] && countBombsOf(i) < CAP) {
                int c = C(px[i], py[i]);
                if (armedDist[c] > 0) { armedDist[c] = 0; shadowQ[qt++] = c; }
            }
        while (qh < qt) {
            int c = shadowQ[qh++];
            int x = CX(c), y = CY(c);
            for (int d = 0; d < 4; ++d) {
                int nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                int nc = C(nx, ny);
                if (armedDist[nc] > armedDist[c] + 1) {
                    armedDist[nc] = armedDist[c] + 1;
                    shadowQ[qt++] = nc;
                }
            }
        }
    }
    buildPessBurn(armedDist, h);

    simulate(bombs, h, base);
    reachSearch(mx, my, base, h, mine, bodyBlock);
    reachSearch(mx, my, base, h, mineH, bodyBlock, nullptr, nullptr, true);
    reachSearch(mx, my, base, h, minePs, bodyBlock, shadowDist, pessBurn);

    vector<BombH> withMine;
    int myBombIdx = -1;
    if (canPlace) {
        withMine = bombs;
        withMine.push_back({ME, mx, my, T + FUSE});
        myBombIdx = (int)withMine.size() - 1;
        simulate(withMine, h, placeW, myBombIdx, flameMine);
        reachSearch(mx, my, placeW, h, mineP, bodyBlock);
        reachSearch(mx, my, placeW, h, mineHP, bodyBlock, nullptr, nullptr, true);
        reachSearch(mx, my, placeW, h, minePsP, bodyBlock, shadowDist, pessBurn);
    }

    // --- Opponent analysis --------------------------------------------------
    // Base world: their doom layer and horizon freedom (for trappability).
    int enemyAliveBase[MAXP], enemyLastBase[MAXP], enemyFreeBase[MAXP];
    static Reach er;
    for (int i = 0; i < P; ++i) {
        if (i == ME || !aliveP[i]) { enemyAliveBase[i] = 0; enemyLastBase[i] = -1; enemyFreeBase[i] = 0; continue; }
        memset(bodyBlockE, 0, N2);
        for (int j = 0; j < P; ++j)
            if (j != i && aliveP[j]) bodyBlockE[C(px[j], py[j])] = 1;
        reachSearch(px[i], py[i], base, h, er, bodyBlockE);
        enemyLastBase[i] = er.lastLayer;
        enemyAliveBase[i] = (er.lastLayer >= h) ? 1 : 0;
        enemyFreeBase[i] = er.horizonAlive;
    }

    // Placement world: kills are enemies whose survival search collapses
    // only after my bomb joins the board.
    double killGain = 0.0;
    int crateGain = 0;
    if (canPlace) {
        crateGain = max(0, placeW.cratesDestroyed - base.cratesDestroyed);
        for (int i = 0; i < P; ++i) {
            if (i == ME || !aliveP[i]) continue;
            memset(bodyBlockE, 0, N2);
            for (int j = 0; j < P; ++j)
                if (j != i && aliveP[j]) bodyBlockE[C(px[j], py[j])] = 1;
            reachSearch(px[i], py[i], placeW, h, er, bodyBlockE);
            int kP = er.lastLayer;
            if (kP >= h) continue;                       // still escapes
            if (enemyAliveBase[i]) {
                double q = 0.30;
                int eL = placeW.trackExplodeLayer;
                if (eL >= 0 && (kP == eL || kP == eL + 1)) {
                    int covered = 0, total = 0;
                    for (int c = 0; c < N2; ++c)
                        if (er.R[kP][c]) {
                            ++total;
                            if (flameMine[kP][c]) ++covered;
                        }
                    double cov = total ? (double)covered / total : 0.0;
                    q = (cov >= 0.999) ? 1.0 : 0.45 + 0.5 * cov;
                }
                killGain += KILL_W * q;
            } else if (kP < enemyLastBase[i]) {
                killGain += 0.3 * KILL_W;                // dies noticeably sooner
            }
        }
    }

    // --- Counterfactual threat worlds ---------------------------------------
    // The nearest armed rival may drop a bomb now, or step one cell and drop
    // next turn. Destinations are only "cautious-safe" if they survive every
    // such placement (the rival's cell plus its open neighbours).
    int threat = -1, threatDist = 99;
    for (int i = 0; i < P; ++i) {
        if (i == ME || !aliveP[i]) continue;
        if (countBombsOf(i) >= CAP) continue;
        if (bombAt(px[i], py[i])) continue;
        int dist = abs(px[i] - mx) + abs(py[i] - my);
        if (dist <= 5 && dist < threatDist) { threatDist = dist; threat = i; }
    }
    bool haveCaut = false, haveCautP = false;
    static uint8_t cautRow[MAXCELLS], cautRowP[MAXCELLS];
    memset(cautRow, 1, N2);
    memset(cautRowP, 1, N2);
    if (threat >= 0 && ms() < 20) {
        int tx = px[threat], ty = py[threat];
        for (int cd = 0; cd < 5; ++cd) {
            int bx = tx + DX[cd], by = ty + DY[cd];
            if (cd < 4 && (bx < 0 || by < 0 || bx >= N || by >= N)) continue;
            if (cd < 4 && board[by][bx] != '.') continue;
            if (cd < 4 && bx == mx && by == my) continue;  // my cell: occupied
            if (bombAt(bx, by)) continue;
            vector<BombH> tb = bombs;
            tb.push_back({threat, bx, by, T + FUSE});
            simulate(tb, h, threatB);
            reachSearch(mx, my, threatB, h, mineT, bodyBlock);
            haveCaut = true;
            for (int c = 0; c < N2; ++c)
                if (!mineT.B[1][c]) cautRow[c] = 0;
            if (canPlace && ms() < 22) {
                tb.push_back({ME, mx, my, T + FUSE});
                simulate(tb, h, threatP);
                reachSearch(mx, my, threatP, h, mineTP, bodyBlock);
                haveCautP = true;
                for (int c = 0; c < N2; ++c)
                    if (!mineTP.B[1][c]) cautRowP[c] = 0;
                if (debugMode)
                    fprintf(stderr, "  tworld bomb@(%d,%d) lastP=%d cautRowP_R=%d\n",
                            bx, by, mineTP.lastLayer,
                            (int)cautRowP[C(mx + 1, my)]);
            }
            if (ms() > 26) break;
        }
        // Keep one concrete threat world for the doomed-depth tie-break.
        vector<BombH> tb = bombs;
        tb.push_back({threat, tx, ty, T + FUSE});
        simulate(tb, h, threatB);
        reachSearch(mx, my, threatB, h, mineT, bodyBlock);
        if (debugMode)
            fprintf(stderr, "  threat=%d at(%d,%d) cautB=%d cautP=%d\n", threat,
                    tx, ty, (int)cautRow[mc], canPlace ? (int)cautRowP[mc] : -1);
    }

    // --- Steering ------------------------------------------------------------
    for (int i = 0; i < N2; ++i) distGoal[i] = 0;
    if (ms() < 30) {
        static int distMe[MAXCELLS];
        bfsFrom(mc, base.entry[0], distMe);

        int haveSlot = (myActive < CAP) ? 1 : 0;
        double intw = 1.5;
        if (T > SHRINK - 80)
            intw = 1.5 + 7.5 * min(1.0, (T - (SHRINK - 80)) / 80.0);

        static uint8_t reachSoon[MAXCELLS];
        memset(reachSoon, 0, N2);
        int kmax = min(6, h);
        for (int k = 0; k <= kmax; ++k)
            for (int c = 0; c < N2; ++c)
                if (mine.R[k][c]) reachSoon[c] = 1;

        double bestV = -1e18;
        int bestC = -1;
        for (int c = 0; c < N2; ++c) {
            if (distMe[c] < 0 || !reachSoon[c]) continue;
            if (board[CY(c)][CX(c)] != '.') continue;
            int x = CX(c), y = CY(c);
            double v = 240.0 * min(5, blastCrates(x, y));
            if (haveSlot) {
                for (int i = 0; i < P; ++i) {
                    if (i == ME || !aliveP[i]) continue;
                    if (!alignedClear(x, y, px[i], py[i])) continue;
                    double trapW = enemyAliveBase[i]
                        ? max(0.4, min(1.6, 1.6 - 0.004 * enemyFreeBase[i]))
                        : 0.0;
                    v += 300.0 * trapW;
                }
            }
            int freedom = 0;
            for (int d = 0; d < 4; ++d) {
                int nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                if (base.entry[0][C(nx, ny)]) ++freedom;
            }
            v += 40.0 * freedom;
            int life = min(closedAt[c], LIMIT) - T;
            v += intw * min(90, life);
            if (c == goalCell) v += 45.0;                // hysteresis
            v -= 55.0 * distMe[c];
            if (v > bestV) { bestV = v; bestC = c; }
        }
        if (bestC >= 0) {
            goalCell = bestC;
            bfsFrom(goalCell, base.entry[0], distGoal);
        } else {
            goalCell = -1;
        }
    }

    // --- Opponent pressure from a drop right here ----------------------------
    // Bombing for pressure alone is rarely worth the self-risk once the arena
    // starts closing: no crates remain and rooms are tight.
    double pressure = 0.0;
    if (canPlace) {
        for (int i = 0; i < P; ++i) {
            if (i == ME || !aliveP[i]) continue;
            if (!alignedClear(mx, my, px[i], py[i])) continue;
            double w = enemyAliveBase[i]
                ? max(1.0, min(2.0, 2.0 - 0.002 * enemyFreeBase[i]))
                : 0.0;
            pressure += w;
        }
        if (T >= SHRINK) pressure *= 0.3;
    }

    // --- Score every candidate action ----------------------------------------
    // Placing must beat the best empty-handed action by PLACE_INERTIA: a
    // lone crate or a whiff of pressure is not worth any escape-margin loss,
    // while a genuine kill chance clears the bar easily.
    const double PLACE_INERTIA = 1500.0;
    bool mustMove = base.burn[0][mc] == 1;
    double bestNP = -1e18, bestP = -1e18;
    int bdNP = 4, bdP = 4;
    for (int place = 0; place <= (canPlace ? 1 : 0); ++place) {
        const World& w = place ? placeW : base;
        const Reach& mr = place ? mineP : mine;
        const Reach& mps = place ? minePsP : minePs;
        for (int d = 0; d < 5; ++d) {
            int nx = mx + DX[d], ny = my + DY[d];
            int dest = mc;
            if (d < 4 && nx >= 0 && ny >= 0 && nx < N && ny < N && w.entry[0][C(nx, ny)])
                dest = C(nx, ny);
            double s = 0.0;
            if (mr.B[1][dest]) {
                s += SAFE_W;
            } else {
                int depth = forwardDepth(w, 1, dest, h);
                s += DOOM_BASE + DOOM_STEP * depth;
            }
            // Rival-shadow tier: alive at the horizon even if every rival
            // walks straight at my escape route.
            if (mps.B[1][dest]) s += PESS_W;
            if (place && !minePsP.B[1][dest])
                s -= minePs.B[1][dest] ? PESS_PLACE_LOST : PESS_PLACE_RISK;
            // Body-linger tier: alive even if each rival body stays put one
            // extra turn, plugging the corridor it stands in.
            if ((place ? mineHP : mineH).B[1][dest]) s += HOLD_W;
            if (place && !mineHP.B[1][dest])
                s -= mineH.B[1][dest] ? HOLD_PLACE_LOST : HOLD_PLACE_RISK;
            s += WIGGLE_W * min(10, wiggleRoom(w, 1, dest, 4));
            // Armed-rival proximity: too small to override any safety tier,
            // but it breaks the ties where steering alone decides -- and a
            // crate goal must not out-vote standing next to a bombing rival.
            // Only on crowded boards: with few players, close-range play is
            // the norm and a standing repulsion just cedes territory.
            int ad = 9;
            if (ad < 4) s -= PROX_W * (4 - ad);
            if (place) {
                s += killGain + CRATE_W * crateGain + PRESSURE_W * pressure;
            }
            const uint8_t* cr = place ? cautRowP : cautRow;
            bool cautKnown = place ? haveCautP : haveCaut;
            if (cautKnown) {
                if (cr[dest]) {
                    s += CAUT_B;
                } else if (mr.B[1][dest]) {
                    s -= CAUT_P;
                    // Among equally threatened options, buy the most turns
                    // inside the primary threat world and keep armed rivals
                    // at arm's length. Capped so it orders threatened dests
                    // without ever cancelling the caution penalty itself.
                    s += 1.2e5 * min(12, forwardDepth(threatB, 1, dest, h));
                    int sep = 8;
                    for (int i = 0; i < P; ++i) {
                        if (i == ME || !aliveP[i]) continue;
                        if (countBombsOf(i) >= CAP) continue;
                        int dd = abs(px[i] - CX(dest)) + abs(py[i] - CY(dest));
                        sep = min(sep, dd);
                    }
                    s += 2500.0 * sep;
                }
            }
            if (distGoal[mc] >= 0 && distGoal[dest] >= 0)
                s += STEER_W * (double)(distGoal[mc] - distGoal[dest]);
            for (int i = 0; i < recentLen; ++i)
                if (recentCells[i] == dest) s -= 130.0;
            if (d < 4) {
                bool contested = false;
                for (int i = 0; i < P; ++i) {
                    if (i == ME || !aliveP[i]) continue;
                    if (abs(px[i] - nx) + abs(py[i] - ny) <= 1) { contested = true; break; }
                }
                if (contested) {
                    s -= 900.0;
                    if (mustMove) s -= 9000.0;
                }
            }
            s -= 0.001 * d;                              // deterministic tie-break
            if (debugMode)
                fprintf(stderr, "  cand p=%d d=%c dest=(%d,%d) safe=%d caut=%d score=%.0f\n",
                        place, DCH[d], CX(dest), CY(dest), (int)mr.B[1][dest],
                        cautKnown ? (int)cr[dest] : -1, s);
            if (place) {
                if (s > bestP) { bestP = s; bdP = d; }
            } else {
                if (s > bestNP) { bestNP = s; bdNP = d; }
            }
        }
    }
    int bd = bdNP, bp = 0;
    if (canPlace && bestP > bestNP + PLACE_INERTIA) { bd = bdP; bp = 1; }

    // Bookkeeping for the next turn's steering terms.
    {
        int nx = mx + DX[bd], ny = my + DY[bd];
        int ndest = mc;
        if (bd < 4 && nx >= 0 && ny >= 0 && nx < N && ny < N && base.entry[0][C(nx, ny)])
            ndest = C(nx, ny);
        if (recentLen < 8) recentCells[recentLen++] = ndest;
        else {
            memmove(recentCells, recentCells + 1, sizeof(int) * 7);
            recentCells[7] = ndest;
        }
    }

    outDir = bd;
    outPlace = bp;
}

// ---------------------------------------------------------------------------
// Protocol loop.
// ---------------------------------------------------------------------------

// Optional stderr trace of one decision (local development only).
static void decideDebug(int chosenD, int chosenP) {
    int mx = px[ME], my = py[ME], mc = C(mx, my);
    int h = min(HORIZON, max(1, LIMIT - T));
    static World w;
    simulate(bombs, h, w);
    static Reach r;
    reachSearch(mx, my, w, h, r);
    fprintf(stderr, "TURN %d me=(%d,%d) lastLayer=%d aliveH=%d chose=%c%d\n",
            T, mx, my, r.lastLayer, r.horizonAlive, DCH[chosenD], chosenP);
    for (int d = 0; d < 5; ++d) {
        int nx = mx + DX[d], ny = my + DY[d];
        int dest = mc;
        if (d < 4 && nx >= 0 && ny >= 0 && nx < N && ny < N && w.entry[0][C(nx, ny)])
            dest = C(nx, ny);
        fprintf(stderr, "  d=%c dest=(%d,%d) B1=%d depth=%d", DCH[d],
                CX(dest), CY(dest), (int)r.B[1][dest], forwardDepth(w, 1, dest, h));
        if (countBombsOf(ME) < CAP && !bombAt(mx, my)) {
            vector<BombH> b2 = bombs;
            b2.push_back({ME, mx, my, T + FUSE});
            static World pw;
            static Reach rp;
            simulate(b2, h, pw);
            reachSearch(mx, my, pw, h, rp);
            int destP = mc;
            if (d < 4 && nx >= 0 && ny >= 0 && nx < N && ny < N && pw.entry[0][C(nx, ny)])
                destP = C(nx, ny);
            fprintf(stderr, " | place: B1=%d depth=%d", (int)rp.B[1][destP],
                    forwardDepth(pw, 1, destP, h));
        }
        fprintf(stderr, "\n");
    }
    for (int k = 0; k <= h; ++k) {
        fprintf(stderr, "  k=%2d now=%3d | R:", k, T + k);
        for (int c = 0; c < N2; ++c)
            if (r.R[k][c]) fprintf(stderr, " (%d,%d)", CX(c), CY(c));
        fprintf(stderr, "\n");
    }
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    debugMode = (argc > 1 && string(argv[1]) == "--debug");

    string tag;
    if (!(cin >> tag)) return 0;
    if (tag != "INIT") return 0;
    cin >> N >> P >> ME >> LIMIT >> FUSE >> RANGE >> CAP >> FIRE >> SHRINK >> EVERY;
    if (!cin || N < 3 || N > MAXN || P < 2 || P > MAXP) return 0;
    N2 = N * N;
    precomputeGeometry();
    goalCell = -1;
    recentLen = 0;
    memset(recentCells, 0, sizeof(recentCells));

    while (cin >> tag) {
        if (tag != "TURN") break;
        cin >> T;
        cin >> tag;  // BOARD
        for (int y = 0; y < N; ++y) cin >> board[y];
        for (int i = 0; i < P; ++i) {
            int id;
            cin >> tag >> id;
            if (id < 0 || id >= P) return 0;
            cin >> px[id] >> py[id];
            int a; cin >> a;
            aliveP[id] = (a != 0);
        }
        int k;
        cin >> tag >> k;  // BOMBS
        bombs.assign(k, BombH());
        for (int i = 0; i < k; ++i)
            cin >> tag >> bombs[i].owner >> bombs[i].x >> bombs[i].y >> bombs[i].at;
        cin >> tag >> k;  // FLAMES
        for (int c = 0; c < N2; ++c) flameEnd0[c] = -1;
        for (int i = 0; i < k; ++i) {
            int x, y, e;
            cin >> tag >> x >> y >> e;
            if (x >= 0 && y >= 0 && x < N && y < N) flameEnd0[C(x, y)] = e;
        }
        cin >> tag >> k;  // STATUS
        for (int i = 0; i < k; ++i) {
            int id, dth, kn, kd;
            cin >> tag >> id >> dth >> kn >> kd;
        }
        cin >> tag >> k;  // SOURCES
        for (int i = 0; i < k; ++i) {
            int x, y, ow, e;
            cin >> tag >> x >> y >> ow >> e;
        }
        cin >> tag;  // END

        if (!cin) break;
        int d = 4, place = 0;
        decide(d, place);
        if (debugMode) decideDebug(d, place);
        printf("%d %c %d\n", T, DCH[d], place ? 1 : 0);
        fflush(stdout);
    }
    return 0;
}
