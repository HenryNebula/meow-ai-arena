// Capture-the-Flag bot, written from scratch for meow-ai-arena episode 3.
// Fully original code; only the public protocol from RULES.md is used.
//
// Strategy outline:
//   * At init, precompute BFS distance fields from every team's base and from
//     every flag spot (walls block; the map never changes).
//   * Each turn: predict this turn's respawns exactly (the engine's spawn rule
//     is deterministic), build an enemy threat map, then assign roles:
//       - carrier: run the flag home along the home field, avoiding threats
//       - escort:  a threatened carrier claims a nearby unit (within 4 steps)
//                  to trail it and zone out chasers
//       - fetcher: claim a reachable flag (optimal injective assignment,
//                  including a fallback value for not fetching at all)
//       - guard:   back up a contested fetch, but only if already close by --
//                  distant units keep their spot coverage instead
//       - camper:  hold an empty flag spot (nearest first, the central race
//                  zone gets priority) so respawns become instant grabs
//   * Survival gate: a chaser within strike range at pickup kills any carrier
//     needing more than two turns home, so units only grab when they can
//     actually make it; otherwise they stand ON the flag cell, which blocks
//     enemy pickups just as well, and fight.
//   * Movement: score the five candidate steps (progress toward the goal
//     field, minus threat penalties, hold-to-fight bonus near contested goals
//     in crowded games) with friendly cell reservations so our own units never
//     block each other.
//   * Actions: pick up a flag we stand on (unless the grab is suicidal);
//     otherwise attack the best enemy in range after our move (carriers and
//     respawning units cannot attack), preferring enemy carriers, low-HP
//     targets and focus fire.

#include <bits/stdc++.h>
using namespace std;

// ---------------------------------------------------------------- protocol

struct PUnit { int id, team, x, y, hp, flag, rat; };    // rat = respawn_at
struct PFlag { int id, x, y, holder, rat; string st; }; // st = home/dropped/carried/cooldown

static int TEAMS, ME, N, TURNS, HP0, DMG, RESPAWN, FLAGRET, FLAGCOOL;
static vector<char> isWall;            // N*N
static vector<int> baseCX, baseCY;     // base centres
static vector<int> spotX, spotY;       // flag spots
static vector<vector<int>> spawnOrder; // [team] 9 base cells in engine spawn order
static vector<vector<int>> homeF;      // [team] BFS distance to that team's base
static vector<vector<int>> spotF;      // [spot] BFS distance to that spot

// Tunable weights (override with -D for experiments; defaults are final).
#ifndef P_FALLBACK
#define P_FALLBACK 26      // cost of not fetching (value of camping etc.)
#endif
#ifndef P_CARRY_BASE
#define P_CARRY_BASE 1     // carry-home exposure (per 10 carry steps), base
#endif
#ifndef P_CARRY_TEAM
#define P_CARRY_TEAM 1     // carry-home exposure per 10 steps, per side count
#endif
#ifndef P_RACE_SKIP
#define P_RACE_SKIP 4      // enemy closer by this many steps -> skip flag
#endif
#ifndef P_RACE_PEN
#define P_RACE_PEN 6       // enemy slightly closer -> penalty
#endif
#ifndef P_CAMP_MAXD
#define P_CAMP_MAXD 12     // don't camp spots farther than this
#endif
#ifndef P_ESCORT_MAXD
#define P_ESCORT_MAXD 4    // free unit must be this close to escort a carrier
#endif
#ifndef P_THREAT
#define P_THREAT 8         // movement penalty per threat attacker
#endif
#ifndef P_THREAT_HURT
#define P_THREAT_HURT 16   // ... when hp <= 68
#endif
#ifndef P_THREAT_CARRY
#define P_THREAT_CARRY 30  // ... when carrying a flag
#endif
#ifndef P_STAND_FIGHT
#define P_STAND_FIGHT 8    // hold position to hit an enemy near a contested goal
#endif
#ifndef P_STAND_FIGHT_MIN
#define P_STAND_FIGHT_MIN 4 // ... but only with this many teams (2p is tempo)
#endif
#ifndef P_GUARD_MAXD
#define P_GUARD_MAXD 4     // only units already this close back up a contested fetch
#endif
#ifndef P_CAMP_CENTER
#define P_CAMP_CENTER 3    // camp-spot discount for the central race zone
#endif

static inline bool inB(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }
static inline bool wallAt(int x, int y) { return !inB(x, y) || isWall[y * N + x]; }
static inline int cellOf(int x, int y) { return y * N + x; }

// BFS distance from a set of source cells (4-neighbour, walls block).
static vector<int> bfsField(const vector<int>& srcs) {
    vector<int> d(N * N, -1);
    static vector<int> q;
    q.clear();
    q.reserve(N * N);
    for (int s : srcs)
        if (!isWall[s] && d[s] < 0) { d[s] = 0; q.push_back(s); }
    const int DX[4] = {1, -1, 0, 0}, DY[4] = {0, 0, 1, -1};
    for (size_t h = 0; h < q.size(); ++h) {
        int c = q[h], cx = c % N, cy = c / N;
        for (int k = 0; k < 4; ++k) {
            int nx = cx + DX[k], ny = cy + DY[k];
            if (!inB(nx, ny)) continue;
            int ni = ny * N + nx;
            if (!isWall[ni] && d[ni] < 0) { d[ni] = d[c] + 1; q.push_back(ni); }
        }
    }
    return d;
}

// ---------------------------------------------------------------- per turn

static int turnNo;
static vector<PUnit> units;
static vector<PFlag> flags;

static bool parseInput() {
    string w;
    while (cin >> w) {
        if (w == "INIT") {
            cin >> TEAMS >> ME >> N >> TURNS >> HP0 >> DMG >> RESPAWN >> FLAGRET >> FLAGCOOL;
        } else if (w == "MAP") {
            isWall.assign(N * N, 0);
            for (int y = 0; y < N; ++y) {
                string row; cin >> row;
                for (int x = 0; x < N; ++x) isWall[y * N + x] = (row[x] == '#');
            }
        } else if (w == "BASES") {
            int k; cin >> k; baseCX.resize(k); baseCY.resize(k);
            for (int i = 0; i < k; ++i) cin >> baseCX[i] >> baseCY[i];
        } else if (w == "SPOTS") {
            int k; cin >> k; spotX.resize(k); spotY.resize(k);
            for (int i = 0; i < k; ++i) cin >> spotX[i] >> spotY[i];
        } else if (w == "TURN") {
            cin >> turnNo;
        } else if (w == "SCORE") {
            vector<int> sc(TEAMS);
            for (auto& s : sc) cin >> s;   // score is not used by this bot
        } else if (w == "UNITS") {
            int k; cin >> k; units.resize(k);
            for (auto& u : units) { string tag; cin >> tag >> u.id >> u.team >> u.x >> u.y >> u.hp >> u.flag >> u.rat; }
        } else if (w == "FLAGS") {
            int k; cin >> k; flags.resize(k);
            for (auto& f : flags) { string tag; cin >> tag >> f.id >> f.st >> f.x >> f.y >> f.holder >> f.rat; }
        } else if (w == "EVENTS") {
            int k; cin >> k;
            string line; getline(cin, line);   // finish the count line
            for (int i = 0; i < k; ++i) getline(cin, line);   // events are not needed
        } else if (w == "END") {
            return true;
        }
    }
    return false;
}

static void initStatics() {
    spawnOrder.assign(TEAMS, {});
    for (int t = 0; t < TEAMS; ++t) {
        int bx = baseCX[t], by = baseCY[t], ccx = N / 2, ccy = N / 2;
        vector<array<int, 3>> cells;   // {cell, dist-to-centre, engine tiebreak}
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                int x = bx + dx, y = by + dy;
                cells.push_back({cellOf(x, y),
                                 abs(x - ccx) + abs(y - ccy),
                                 abs(x - bx) + 2 * abs(y - by)});
            }
        // The engine sorts stably by (dist to centre, |dx|+2|dy|); the order
        // above matches its enumeration order, so ties keep the same order.
        stable_sort(cells.begin(), cells.end(), [](const array<int, 3>& a, const array<int, 3>& b) {
            if (a[1] != b[1]) return a[1] < b[1];
            return a[2] < b[2];
        });
        for (auto& c : cells) spawnOrder[t].push_back(c[0]);
    }
    homeF.assign(TEAMS, {});
    for (int t = 0; t < TEAMS; ++t) homeF[t] = bfsField(spawnOrder[t]);
    spotF.assign(spotX.size(), {});
    for (size_t s = 0; s < spotX.size(); ++s) spotF[s] = bfsField({cellOf(spotX[s], spotY[s])});
}

// Mirror of the engine's attack check (positions are post-movement).
static bool canAttack(int ax, int ay, int bx, int by) {
    int dx = bx - ax, dy = by - ay;
    if (abs(dx) + abs(dy) > 2) return false;
    if (dx == 2 && dy == 0) return !wallAt(ax + 1, ay);
    if (dx == -2 && dy == 0) return !wallAt(ax - 1, ay);
    if (dy == 2 && dx == 0) return !wallAt(ax, ay + 1);
    if (dy == -2 && dx == 0) return !wallAt(ax, ay - 1);
    if (abs(dx) == 1 && abs(dy) == 1) return !wallAt(ax + dx, ay) || !wallAt(ax, ay + dy);
    return true;
}

// Could unit `uid` survive picking up a flag at (cx, cy)?  A chaser within
// strike range at pickup keeps pace with the carrier and lands a hit every
// turn, so any carrier needing more than 2 turns home dies -- unless nearby
// friends can drop the chaser first.  Declining is cheap: a unit standing
// on the flag cell without picking it up blocks every enemy pickup too.
static bool survivalOk(int uid, int cx, int cy) {
    int H = homeF[ME][cellOf(cx, cy)];
    if (H < 0 || H <= 2) return true;          // home before the third hit
    int strikers = 0, covered = 0;
    for (const PUnit& e : units) {
        if (e.team == ME || e.x < 0) continue; // dead units cannot strike
        if (abs(e.x - cx) + abs(e.y - cy) > 2) continue;
        ++strikers;
        int sup = 0;
        for (const PUnit& a : units)
            if (a.team == ME && a.id != uid && a.x >= 0 &&
                !(a.rat != -1 && a.rat <= turnNo) &&
                abs(a.x - e.x) + abs(a.y - e.y) <= 2) ++sup;
        if (sup >= 2) ++covered;               // friends drop it within 2 turns
    }
    if (strikers == 0) return true;
    return strikers == 1 && covered == 1 && units[uid].hp > 2 * DMG;
}

// ---------------------------------------------------------------- decide

struct Plan {
    bool active = false;      // unit exists this turn
    int x = -1, y = -1;       // effective position (predicted respawn included)
    bool respawning = false;  // spawns this turn: immune, cannot attack
    bool carrier = false;
    const vector<int>* fld = nullptr;  // goal distance field
    int flagCell = -1;        // goal flag cell (grab bonus), else -1
    double thrW = 8;          // weight per threat attacker at a candidate cell
    const char* role = "none";
    int flagId = -1;          // flag id being fetched (for stickiness)
    int moveDir = 0;
    int nx = -1, ny = -1;     // position after the chosen move
};

static const int MDX[5] = {0, 0, 0, -1, 1};
static const int MDY[5] = {0, -1, 1, 0, 0};
static const char MCH[5] = {'S', 'U', 'D', 'L', 'R'};

static uint64_t rngState = 88172645463325252ULL;
static double jitter(int a, int b, int c) {
    rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17;
    uint64_t h = rngState ^ (uint64_t)(a * 131 + b * 37 + c * 977);
    return (double)(h % 2000) / 1000.0 - 1.0;   // [-1, 1)
}

// Per-turn pool of freshly computed fields (stable addresses).
static vector<unique_ptr<vector<int>>> fieldPool;
static const vector<int>* pooledField(int cell) {
    fieldPool.push_back(make_unique<vector<int>>(bfsField({cell})));
    return fieldPool.back().get();
}
static const vector<int>* pooledFieldFrom(const vector<int>& srcs) {
    fieldPool.push_back(make_unique<vector<int>>(bfsField(srcs)));
    return fieldPool.back().get();
}

static string decide() {
    fieldPool.clear();
    Plan plan[3];
    vector<char> reserved(N * N, 0);

    // ---- 1. effective positions with exact respawn prediction -------------
    // The respawn phase runs at the start of this turn, in unit id order.
    vector<int> px(units.size(), -1), py(units.size(), -1);
    {
        set<int> occ;
        for (auto& u : units)
            if (u.x >= 0) { px[u.id] = u.x; py[u.id] = u.y; occ.insert(cellOf(u.x, u.y)); }
        for (auto& u : units)
            if (u.x < 0 && u.rat != -1 && u.rat <= turnNo) {
                for (int c : spawnOrder[u.team])
                    if (!occ.count(c)) {
                        occ.insert(c);
                        px[u.id] = c % N; py[u.id] = c / N;
                        break;
                    }
            }
    }

    // ---- 2. threat map: enemies that could hit each cell next turn ---------
    // A unit that moves 1 and attacks with range 2 threatens cells within
    // Manhattan 3. Carriers and units spawning this turn cannot attack.
    vector<int> threat(N * N, 0);   // scaled by 2
    for (auto& u : units)
        if (u.team != ME && px[u.id] >= 0 && u.flag < 0 && !(u.x < 0 && u.rat <= turnNo)) {
            int add = (u.hp <= DMG) ? 1 : 2;   // nearly-dead enemies scare less
            for (int dx = -3; dx <= 3; ++dx)
                for (int dy = -3; dy <= 3; ++dy)
                    if (abs(dx) + abs(dy) <= 3 && inB(px[u.id] + dx, py[u.id] + dy))
                        threat[cellOf(px[u.id] + dx, py[u.id] + dy)] += add;
        }
    // path distance to the nearest enemy (carrier safety / escort choice)
    vector<int> srcs;
    for (auto& u : units)
        if (u.team != ME && px[u.id] >= 0) srcs.push_back(cellOf(px[u.id], py[u.id]));
    const vector<int>& enemyNear = *pooledFieldFrom(srcs);

    // ---- 3. my units --------------------------------------------------------
    vector<int> mine;   // unit ids of my side, in id order
    for (auto& u : units)
        if (u.team == ME) mine.push_back(u.id);
    for (int k = 0; k < 3; ++k) {
        auto& u = units[mine[k]];
        Plan& p = plan[k];
        p.x = px[u.id]; p.y = py[u.id];
        p.active = p.x >= 0;
        p.respawning = (u.x < 0);
        p.carrier = u.flag >= 0;
        p.thrW = p.carrier ? P_THREAT_CARRY
                           : (u.hp <= 2 * DMG ? P_THREAT_HURT : P_THREAT);
    }

    // ---- 4. flags pickable this turn ----------------------------------------
    struct Tgt { int cell; const vector<int>* fld; int danger; int fid; };
    vector<Tgt> tgts;
    for (auto& f : flags) {
        if (f.st != "home" && f.st != "dropped") continue;
        if (f.st == "dropped" && f.rat == turnNo) continue;   // returns right now
        int c = cellOf(f.x, f.y);
        const vector<int>* fld = nullptr;
        for (size_t s = 0; s < spotX.size(); ++s)
            if (spotX[s] == f.x && spotY[s] == f.y) { fld = &spotF[s]; break; }
        if (!fld) fld = pooledField(c);   // dropped away from every spot
        tgts.push_back({c, fld, threat[c] / 2, f.id});
    }

    // ---- 5. role assignment --------------------------------------------------
    int freeK[3], freeN = 0;
    for (int k = 0; k < 3; ++k)
        if (plan[k].active && !plan[k].carrier && !plan[k].respawning) freeK[freeN++] = k;

    // 5a. carriers run home; threatened carriers claim an escort first,
    // because a carrier chased from within range 2 dies before delivering
    int carrierK[3], carrierN = 0;
    for (int k = 0; k < 3; ++k)
        if (plan[k].active && plan[k].carrier) {
            plan[k].fld = &homeF[ME];
            plan[k].role = "carry";
            carrierK[carrierN++] = k;
        }
    for (int ci = 0; ci < carrierN && freeN > 0; ++ci) {
        int k = carrierK[ci];
        int d = enemyNear[cellOf(plan[k].x, plan[k].y)];
        if (d < 0 || d > 3) continue;      // no danger, no escort
        if (d > 2 && units[mine[k]].hp > 2 * DMG) continue;   // mild danger on a healthy carrier: keep fetching
        int best = -1, bd = INT_MAX;
        for (int j = 0; j < freeN; ++j) {
            int jk = freeK[j];
            int dd = abs(plan[jk].x - plan[k].x) + abs(plan[jk].y - plan[k].y);
            if (dd < bd) { bd = dd; best = j; }
        }
        // a distant "escort" is a farmer abandoning its spot and never
        // catches up anyway; only a unit already close guards the carry
        if (bd > P_ESCORT_MAXD) continue;
        // the escort trails the carrier: any chaser coming from behind ends
        // up next to the escort and gets hit every turn
        plan[freeK[best]].fld = pooledField(cellOf(plan[k].x, plan[k].y));
        plan[freeK[best]].role = "escort";
        for (int q = best + 1; q < freeN; ++q) freeK[q - 1] = freeK[q];
        --freeN;
    }

    // 5b. fetchers: optimal injective assignment of free units to flags.
    // Leaving a unit for a secondary role (guard/intercept/camp) costs a
    // fixed fallback value; a visible flag is usually worth more than
    // speculatively waiting on an empty spot, so the fallback is generous.
    // Chasing last turn's flag gets a small bonus to stop goal dithering.
    const int FALLBACK = P_FALLBACK;
    static int lastFlag[3] = {-1, -1, -1};   // flag id chased last turn
    static int campAssigned[3] = {-1, -1, -1}; // spot index camped last turn
    int assign[3] = {-1, -1, -1};   // flag index per free-unit slot, -1 = other role
    if (freeN > 0 && !tgts.empty()) {
        int m = (int)tgts.size();
        int bestCost = INT_MAX;
        int cur[3];
        // cost of the unit in free slot i taking flag fi: travel there plus
        // the exposure of the carry home (long carries die in crowded games;
        // the risk per step grows with the number of sides fighting nearby)
        int carryW = P_CARRY_BASE + P_CARRY_TEAM * TEAMS;   // per 10 carry steps
        auto cost = [&](int i, int fi) -> int {
            Plan& p = plan[freeK[i]];
            int d = tgts[fi].fld->at(cellOf(p.x, p.y));
            if (d < 0) return -1;
            int hd = homeF[ME][tgts[fi].cell];
            if (hd < 0) hd = 20;
            int c = d + (hd * carryW) / 10 + 4 * max(0, tgts[fi].danger - 1);
            // race: if an enemy gets there clearly first, skip the flag
            for (auto& u : units)
                if (u.team != ME && px[u.id] >= 0) {
                    int ed = tgts[fi].fld->at(cellOf(px[u.id], py[u.id]));
                    if (ed < 0) continue;
                    if (ed + P_RACE_SKIP <= d) return INT_MAX / 2;
                    if (ed < d) c += P_RACE_PEN;
                }
            if (tgts[fi].fid == lastFlag[freeK[i]]) c -= 3;   // stick with it
            return c;
        };
        function<void(int, int)> rec = [&](int i, int costSoFar) {
            if (costSoFar >= bestCost) return;
            if (i == freeN) {
                bestCost = costSoFar;
                for (int q = 0; q < freeN; ++q) assign[q] = cur[q];
                return;
            }
            cur[i] = -1;   // leave this unit for a guard/intercept/camp role
            rec(i + 1, costSoFar + FALLBACK);
            for (int fi = 0; fi < m; ++fi) {
                bool used = false;
                for (int q = 0; q < i; ++q) if (cur[q] == fi) used = true;
                if (used) continue;
                int c = cost(i, fi);
                if (c < 0) continue;
                cur[i] = fi;
                rec(i + 1, costSoFar + c);
            }
        };
        rec(0, 0);
        for (int i = 0; i < freeN; ++i)
            if (assign[i] >= 0) {
                Plan& p = plan[freeK[i]];
                p.fld = tgts[assign[i]].fld;
                p.flagCell = tgts[assign[i]].cell;
                p.flagId = tgts[assign[i]].fid;
                p.role = "fetch";
            }
    }

    // 5c. leftover units: guard a contested fetch, intercept, camp
    int campSpot[3] = {-1, -1, -1};
    for (int i = 0; i < freeN; ++i) {
        if (assign[i] >= 0) continue;
        Plan& p = plan[freeK[i]];

        // guard: back up a fetcher heading to a contested flag, or converge
        // on a teammate's spot that an enemy is contesting
        const vector<int>* guardFld = nullptr;
        int guardD = INT_MAX;
        for (int q = 0; q < freeN; ++q)
            if (assign[q] >= 0 && tgts[assign[q]].danger >= 1) {
                int d = tgts[assign[q]].fld->at(cellOf(p.x, p.y));
                if (d >= 0 && d < guardD) { guardD = d; guardFld = tgts[assign[q]].fld; }
            }
        if (guardFld && guardD <= P_GUARD_MAXD) {
            p.fld = guardFld;
            p.role = "guard";
            continue;
        }

        // camp: wait on an empty flag spot. Stick with the spot chosen on an
        // earlier turn (within reach, still empty) to avoid goal dithering,
        // and only camp spots that are reasonably close.
        const int CAMP_MAXD = P_CAMP_MAXD;
        int bestS = -1, bestD = INT_MAX;
        {
            int keep = campAssigned[freeK[i]];
            if (keep >= 0 && keep < (int)spotX.size()) {
                bool taken = false;
                for (auto& f : flags)
                    if (f.st == "home" && f.x == spotX[keep] && f.y == spotY[keep]) taken = true;
                for (int q = 0; q < i; ++q)
                    if (campSpot[q] == keep) taken = true;
                int d = spotF[keep][cellOf(p.x, p.y)];
                if (!taken && d >= 0 && d <= CAMP_MAXD) { bestS = keep; bestD = d; }
            }
        }
        if (bestS < 0) {
            // the centre spot is the race zone both sides reach at the same
            // time: parking on it converts every centre respawn into an
            // instant grab instead of a coin-flip walk race
            for (size_t s = 0; s < spotX.size(); ++s) {
                int c = cellOf(spotX[s], spotY[s]);
                bool taken = false;
                for (auto& f : flags)
                    if (f.st == "home" && f.x == spotX[s] && f.y == spotY[s]) taken = true;
                if (taken) continue;
                for (int q = 0; q < i; ++q)
                    if (campSpot[q] == (int)s) taken = true;   // a teammate camps it
                if (taken) continue;
                int d = spotF[s][cellOf(p.x, p.y)];
                if (d < 0 || d > CAMP_MAXD) continue;
                d += threat[c] / 4;
                if (abs(spotX[s] - N / 2) + abs(spotY[s] - N / 2) <= 2)
                    d -= P_CAMP_CENTER;    // central race zone
                if (d < bestD) { bestD = d; bestS = (int)s; }
            }
        }
        campSpot[i] = bestS;
        campAssigned[freeK[i]] = bestS;
        p.fld = (bestS >= 0) ? &spotF[bestS] : &homeF[ME];
        p.role = "camp";
    }

    // ---- 6. movement: score 5 candidate steps, reserve cells -----------------
    for (int k = 0; k < 3; ++k) {
        Plan& p = plan[k];
        if (!p.active) continue;
        const vector<int>& F = p.fld ? *p.fld : homeF[ME];
        int myCell = cellOf(p.x, p.y);
        // local balance around the goal flag: enemies that can hit the flag
        // cell next turn vs. nearby friendly fighters
        double grabBonus = 0;
        bool grabSafe = true;
        if (p.flagCell >= 0 && !p.carrier) {
            // only grab when the carrier can actually make it home; standing
            // on the flag without picking it up blocks enemies just as well
            grabSafe = survivalOk(mine[k], p.flagCell % N, p.flagCell / N);
            grabBonus = grabSafe ? 25.0 : -10.0;
        }
        double bestS = -1e18;
        int bestD = 0;
        for (int d = 0; d < 5; ++d) {
            int cx = p.x + MDX[d], cy = p.y + MDY[d];
            if (wallAt(cx, cy)) { cx = p.x; cy = p.y; }   // wall bump = stay
            int c = cellOf(cx, cy);
            if (reserved[c] && c != myCell) continue;      // avoid friendly block
            double s = jitter(turnNo, mine[k], d);
            int fd = (F[c] >= 0) ? F[c] : 999;
            s -= 10.0 * fd;
            s -= p.thrW * (threat[c] / 2.0);
            if (p.carrier) {
                int ed = enemyNear[c];
                if (ed >= 0 && ed <= 3) s -= (4 - ed) * 8.0;
            }
            if (p.flagCell == c && !p.carrier)   // step onto / hold the flag
                s += (d == 0 && !grabSafe) ? 3.0 : grabBonus;
#if P_STAND_FIGHT
            // fight stance: already in attack range with the goal close and
            // contested -- standing still this turn lands a hit, while walking
            // concedes the exchange
            if (d == 0 && !p.carrier && !p.respawning && TEAMS >= P_STAND_FIGHT_MIN &&
                F[myCell] <= 3 && threat[p.flagCell >= 0 ? p.flagCell : myCell] > 0) {
                bool inRange = false;
                for (auto& e : units)
                    if (e.team != ME && e.x >= 0 &&
                        canAttack(p.x, p.y, e.x, e.y)) { inRange = true; break; }
                if (inRange) s += (double)P_STAND_FIGHT;
            }
#endif
            if (d == 0 && fd > 0) s -= 0.5;                // mild bias to keep moving
            if (s > bestS) { bestS = s; bestD = d; }
        }
        int nx = p.x + MDX[bestD], ny = p.y + MDY[bestD];
        if (wallAt(nx, ny)) { nx = p.x; ny = p.y; }
        p.moveDir = bestD; p.nx = nx; p.ny = ny;
        reserved[cellOf(nx, ny)] = 1;
    }

    // remember this turn's goals for stickiness next turn: a unit keeps its
    // camped spot through fetch/carry cycles so the farm coverage survives
    // role churn; only support roles or dying give the spot up
    for (int k = 0; k < 3; ++k) {
        lastFlag[k] = plan[k].flagId;
        const char* r = plan[k].role;
        if (!plan[k].active || strcmp(r, "camp") != 0) campAssigned[k] = -1;
    }

    // ---- 7. actions ------------------------------------------------------------
    int chosen[3] = {-1, -1, -1};
    string out = to_string(turnNo);
    for (int k = 0; k < 3; ++k) {
        Plan& p = plan[k];
        string mv = p.active ? string(1, MCH[p.moveDir]) : "S";
        string act = "-";
        if (p.active && !p.carrier) {
            bool pickup = false;
            for (auto& f : flags)
                if ((f.st == "home" || (f.st == "dropped" && f.rat != turnNo)) &&
                    f.x == p.nx && f.y == p.ny) { pickup = true; break; }
            if (pickup && survivalOk(mine[k], p.nx, p.ny)) {
                act = "P";                      // suicidal grabs feed the enemy
            } else if (!p.respawning) {
                int bestT = -1;
                double bestSc = -1e18;
                for (auto& e : units) {
                    if (e.team == ME || px[e.id] < 0) continue;
                    if (e.x < 0 && e.rat != -1 && e.rat <= turnNo) continue;   // immune
                    if (!canAttack(p.nx, p.ny, px[e.id], py[e.id])) continue;
                    double sc = 0;
                    if (e.flag >= 0) sc += 120;          // enemy carrier: best target
                    if (e.hp <= DMG) sc += 80;           // kill shot
                    if (abs(px[e.id] - p.nx) + abs(py[e.id] - p.ny) == 1) sc += 25;
                    sc += 3 * DMG - e.hp;                // prefer low HP
                    for (int q = 0; q < k; ++q)
                        if (chosen[q] == e.id) sc += 15; // focus fire
                    if (sc > bestSc) { bestSc = sc; bestT = e.id; }
                }
                if (bestT >= 0) { act = to_string(bestT); chosen[k] = bestT; }
            }
        }
        out += " " + mv + " " + act;
    }
    return out;
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    bool inited = false;
    while (parseInput()) {
        if (!inited) { initStatics(); inited = true; }
        cout << decide() << endl;
    }
}
