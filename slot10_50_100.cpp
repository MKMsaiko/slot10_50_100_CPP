/*
遊戲規則：
Line Game : 25 lines  window : 3*5
線獎由左到右，同符號達3、4、5連線且符合線圖，即有贏分
主遊戲中出現3個以上任意位置的Scatter觸發免費遊戲x5
免費遊戲中出現3個任意位置的Scatter可再觸發，無上限
主遊戲出現｛3，4，5｝個Scatter觸發之免費遊戲，該免費遊戲贏分分別｛x10，x50，x100｝
免費遊戲中再觸發之免費遊戲則依最初主遊戲觸發時之倍率計算
Scatter出現在3-5轉輪，Wild出現在2-5轉輪，可替代除S外之任意符號
該遊戲有兩個賠率表，皆為單線押注1時之表現

程式流程：
依處理器thread數 → 分數個worker → 各自做以下1-7 → 彙整輸出

(1) 轉窗（主遊戲）
        呼叫：spinWindow(rng, reelsMG, &w)
        對 5 軸各抽一個 stop，視窗填入 stop、stop+1、stop+2（三格環迴）。

(2) 算主遊戲線獎
        呼叫：evalAllLines(&w, &payMG)
        走 25 條線，逐條呼叫 linePay(...) 加總。
        linePay：
        湊到　3/4/5 連，回傳 pay[target][len-3]（未乘線注）。

(3) 判 Scatter、決定是否進 FG
        呼叫：countScatter(&w) → 數 5×3 視窗中的 S
        < 3：本把結束；若 mgLine == 0 則 deadSpins++。
        >= 3：觸發 FG，triggerCount++，並決定觸發倍率：
         mul = fgMulByScatter(s) → 3S/4S/5S → ×10/×50/×100

(4) 跑「一整串」FG
        呼叫：playFG(rng, &w) 回傳 (spins, base, retri, zeroBatches, totalBatches)

        playFG 內每一轉：
            spinWindow(rng, reelsFG, &w)
            win = evalAllLines(&w, &payFG) * betPerLine
            若 countScatter(&w) >= 3：queue += 5，retri++
            以 5 轉為一批次，累計 batchWin；若整批為 0，zeroBatches++
            備註：base 為整串 FG 未乘觸發倍率的總派彩

(5) 把整串 FG 派彩加權、累計
        fgWin   = fgBase * mul, spinTotal = mgLine + fgWin...

(6) 更新單把峰值與分層
        ≥20→big，≥60→mega，≥100→super，≥300→holy，≥500→jumbo，≥1000→jojo；更新 maxSingleSpin。

(7) 進度心跳（降低同步成本）
        每把增加 bumpCnt；每達 4096（bump）時; atomic 加到全域 spinsDone（Add 4096），bumpCnt 清零。
        背景心跳執行緒每秒讀 spinsDone，輸出進度/速度/ETA。
*/

#ifdef _WIN32
#include <windows.h> // 把 Windows 主控台碼頁切到 UTF-8（避免 中文/符號 亂碼）
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <functional>
using namespace std;

/**************
 * 參數（可調）
 **************/
static long long numSpins = 1000000000LL;                    // 總轉數（預設 10 億）
static double betPerLine = 0.04;                             // 每線押注
static int numWorkers = (int)thread::hardware_concurrency(); // 併發 worker(視硬體thread數而定)
static double excelRTP = 0.965984;                           // Excel 試算 RTP，於輸出驗證；設負值則不比較

/**************
 * 線數
 **************/
constexpr int numLines = 25;

/**************
 * 符號編碼
 **************/
enum Sym : uint8_t
{
    S9,
    S10,
    SJ,
    SQ,
    SK,
    SR,
    SF,
    SB,
    SW,
    SS,
    NumSymbols
};

/**************
 * 賠率表 pay[符號][streak-3] = 倍率
 * MG：主遊戲；FG：免費遊戲（倍率較高）
 **************/
static array<array<double, 3>, NumSymbols> payMG = []
{
    array<array<double, 3>, NumSymbols> p{};
    p[S9] = {5, 10, 40};
    p[S10] = {5, 15, 50};
    p[SJ] = {10, 15, 75};
    p[SQ] = {10, 20, 100};
    p[SK] = {10, 25, 150};
    p[SB] = {15, 50, 300};
    p[SF] = {25, 100, 500};
    p[SR] = {40, 200, 1500};
    return p;
}();

static array<array<double, 3>, NumSymbols> payFG = []
{
    array<array<double, 3>, NumSymbols> p{};
    p[S9] = {10, 15, 100};
    p[S10] = {10, 25, 125};
    p[SJ] = {15, 30, 150};
    p[SQ] = {15, 40, 175};
    p[SK] = {30, 45, 300};
    p[SB] = {30, 100, 800};
    p[SF] = {50, 200, 1500};
    p[SR] = {100, 500, 3000};
    return p;
}();

/**************
 * 線圖：25 線（0=上,1=中,2=下）
 **************/
static const array<array<uint8_t, 5>, numLines> lines = {{{1, 1, 1, 1, 1}, {0, 0, 0, 0, 0}, {2, 2, 2, 2, 2}, {0, 1, 2, 1, 0}, {2, 1, 0, 1, 2}, {0, 0, 1, 2, 2}, {2, 2, 1, 0, 0}, {1, 2, 2, 2, 1}, {1, 0, 0, 0, 1}, {0, 1, 1, 1, 0}, {2, 1, 1, 1, 2}, {1, 0, 1, 2, 1}, {1, 2, 1, 0, 1}, {0, 0, 2, 2, 0}, {2, 2, 0, 0, 2}, {0, 2, 2, 2, 0}, {2, 0, 0, 0, 2}, {1, 0, 2, 0, 1}, {1, 2, 0, 2, 1}, {0, 1, 0, 1, 0}, {2, 1, 2, 1, 2}, {1, 1, 0, 1, 1}, {1, 1, 2, 1, 1}, {0, 2, 0, 2, 0}, {2, 0, 2, 0, 2}}};

/**************
 * 輪帶（啟動時：字串→符號碼）
 **************/
static vector<vector<string>> reelsMGstr = {
    // Reel 1
    {"10", "Q", "9", "R", "B", "J", "10", "K", "Q", "10", "J", "Q", "10", "J", "B", "Q", "J", "J", "10", "Q", "9", "Q", "Q", "B", "9", "J", "B", "F", "K", "Q", "K", "B", "B", "Q", "B", "10", "J", "Q", "10", "B", "F", "K", "R", "B", "R", "10", "9", "J", "Q"},
    // Reel 2
    {"9", "K", "J", "9", "Q", "B", "9", "K", "B", "9", "K", "9", "9", "W", "10", "J", "R", "B", "10", "Q", "W", "R", "K", "9", "10", "K", "Q", "K", "B", "F", "K", "R", "K", "Q", "B", "K", "9", "B", "F", "10", "R", "Q", "K", "R", "9", "K", "W", "9", "10", "9"},
    // Reel 3
    {"9", "9", "9", "10", "S", "9", "10", "9", "10", "10", "9", "10", "S", "10", "J", "10", "10", "9", "J", "F", "J", "10", "J", "J", "Q", "J", "R", "Q", "Q", "J", "Q", "F", "K", "K", "B", "B", "J", "F", "F", "J", "B", "K", "R", "R", "F", "F", "R", "W", "9", "10", "J"},
    // Reel 4
    {"9", "9", "10", "9", "9", "J", "9", "Q", "9", "9", "10", "10", "9", "10", "10", "J", "J", "9", "J", "J", "Q", "R", "J", "J", "Q", "Q", "9", "Q", "J", "Q", "W", "W", "W", "W", "K", "F", "K", "Q", "B", "B", "W", "W", "W", "W", "R", "J", "B", "K", "Q", "Q", "B", "Q", "F", "K", "10", "S", "S"},
    // Reel 5
    {"9", "9", "10", "10", "Q", "W", "J", "J", "J", "Q", "Q", "K", "W", "Q", "Q", "10", "Q", "K", "K", "Q", "K", "K", "F", "B", "K", "B", "B", "10", "B", "K", "B", "B", "J", "B", "R", "B", "F", "F", "K", "F", "B", "F", "F", "R", "B", "Q", "W", "F", "B", "10", "S", "S"}};

static vector<vector<string>> reelsFGstr = {
    // Reel 1
    {"10", "Q", "B", "10", "Q", "J", "9", "B", "9", "Q", "J", "K", "J", "10", "J", "Q", "J", "B", "Q", "K", "Q", "10", "Q", "B", "Q", "R", "B", "K", "J", "Q", "10", "K", "9", "Q", "B", "R", "J", "Q", "10", "B", "Q", "F", "R", "10", "Q", "10", "9", "J", "Q", "10", "B", "10", "Q", "J", "10", "J", "F", "J", "B", "10", "Q", "J", "B", "Q", "10", "Q", "10", "J"},
    // Reel 2
    {"9", "R", "9", "K", "Q", "B", "K", "J", "F", "B", "9", "K", "9", "B", "W", "K", "9", "J", "K", "W", "Q", "K", "F", "K", "R", "10", "K", "9", "K", "B", "K", "R", "K", "9", "B", "9", "K", "9", "B", "10", "B", "K", "R", "Q", "R", "K", "10", "F", "K", "9", "10", "K", "9", "K", "Q", "K", "R", "9", "K", "9", "K", "F", "R", "9", "K", "10", "Q", "K"},
    // Reel 3
    {"9", "J", "F", "10", "S", "9", "10", "9", "K", "Q", "9", "10", "S", "10", "J", "F", "10", "S", "J", "B", "J", "10", "Q", "R", "J", "R", "Q", "J", "9", "Q", "F", "B", "10", "B", "K", "J", "10", "F", "K", "F", "J", "F", "R", "10", "F", "R", "W", "J", "10", "J", "F", "9", "J", "9", "10", "J", "F", "9", "10"},
    // Reel 4
    {"9", "S", "10", "9", "Q", "J", "S", "Q", "K", "9", "B", "10", "Q", "S", "J", "10", "F", "J", "9", "J", "Q", "J", "R", "Q", "10", "Q", "J", "9", "Q", "J", "9", "Q", "W", "W", "W", "K", "F", "9", "Q", "S", "B", "Q", "10", "J", "K", "R", "10", "B", "J", "Q", "9", "K", "W", "W", "W", "B", "9", "S", "9", "K", "J"},
    // Reel 5
    {"9", "K", "B", "S", "10", "B", "K", "F", "S", "Q", "B", "Q", "J", "K", "W", "F", "Q", "R", "Q", "K", "10", "Q", "F", "9", "F", "B", "K", "B", "S", "10", "J", "K", "B", "10", "J", "B", "R", "B", "S", "F", "K", "F", "B", "F", "K", "10", "B", "Q", "W", "F", "Q", "W", "J", "B", "S", "F", "K", "Q", "K", "F", "S"}};
/**************
 * 啟動時轉碼：string → uint8_t
 **************/
static uint8_t symCode(const string &s)
{
    if (s == "9")
        return S9;
    if (s == "10")
        return S10;
    if (s == "J")
        return SJ;
    if (s == "Q")
        return SQ;
    if (s == "K")
        return SK;
    if (s == "R")
        return SR;
    if (s == "F")
        return SF;
    if (s == "B")
        return SB;
    if (s == "W")
        return SW;
    if (s == "S")
        return SS;
    throw runtime_error("unknown symbol: " + s);
}
static vector<vector<uint8_t>> packReels(const vector<vector<string>> &src)
{
    vector<vector<uint8_t>> dst;
    dst.reserve(src.size());
    for (const auto &col : src)
    {
        vector<uint8_t> v;
        v.reserve(col.size());
        for (const auto &t : col)
            v.push_back(symCode(t));
        dst.push_back(move(v));
    }
    return dst;
}
static vector<vector<uint8_t>> reelsMG, reelsFG;

/**************
 * 轉窗（5×3 視窗；每把覆寫；各worker各自持有）
 **************/
struct Window5x3
{
    uint8_t c[5][3];
};

// 隨機停點 → 視窗取 stop, stop+1, stop+2（環迴）
static inline void spinWindow(mt19937_64 &rng,
                              const vector<vector<uint8_t>> &reels,
                              Window5x3 *w)
{
    for (int r = 0; r < 5; r++)
    {
        int L = (int)reels[r].size();
        int stop = (int)(rng() % L);
        w->c[r][0] = reels[r][stop];
        w->c[r][1] = reels[r][(stop + 1) % L];
        w->c[r][2] = reels[r][(stop + 2) % L];
    }
}

/**************
 * 線獎：左到右；W 可代；S 斷線
 * 回傳該線倍率（未乘線注）
 **************/
static inline double linePay(const Window5x3 *w, const array<uint8_t, 5> &line,
                             const array<array<double, 3>, NumSymbols> *pay)
{
    // 找到第一個「非 W 非 S」作為目標符號
    uint8_t target = 255;
    for (int r = 0; r < 5; r++)
    {
        uint8_t s = w->c[r][line[r]];
        if (s != SW && s != SS)
        {
            target = s;
            break;
        }
    }
    // 從左數連線；遇 S 斷，遇 W 視為匹配
    int cnt = 0;
    for (int r = 0; r < 5; r++)
    {
        uint8_t s = w->c[r][line[r]];
        if (s == SS)
            break;
        if (s == SW || s == target)
            cnt++;
        else
            break;
    }
    if (cnt >= 3)
        return (*pay)[target][cnt - 3];
    return 0.0;
}

// 25 線加總（未乘線注）
static inline double evalAllLines(const Window5x3 *w,
                                  const array<array<double, 3>, NumSymbols> *pay)
{
    double sum = 0.0;
    for (int i = 0; i < numLines; i++)
        sum += linePay(w, lines[i], pay);
    return sum;
}

// 數 S : 3以上觸發 FG
static inline int countScatter(const Window5x3 *w)
{
    int c = 0;
    for (int r = 0; r < 5; r++)
    {
        if (w->c[r][0] == SS)
            c++;
        if (w->c[r][1] == SS)
            c++;
        if (w->c[r][2] == SS)
            c++;
    }
    return c;
}

// 決定 FG 倍率（3S→×10、4S→×50、5S→×100）
static inline double fgMulByScatter(int s)
{
    if (s >= 5)
        return 100;
    if (s == 4)
        return 50;
    if (s == 3)
        return 10;
    return 0;
}

/**************
 * 一整串 FG（5 轉起始；再觸發+5 轉，無上限）
 * 回傳：spins(總轉數)、base(FG 未乘倍率之總派彩)、
 *       retri(再觸發次數)、zeroBatches(5轉全空批次數)、totalBatches(總批次)
 **************/
static tuple<int, double, int, int, int>
playFG(mt19937_64 &rng, Window5x3 *w)
{
    int queue = 5;
    int spins = 0, retri = 0, batchSpin = 0, zeroBatches = 0, totalBatches = 0;
    double base = 0.0, batchWin = 0.0;

    while (queue > 0)
    {
        queue--;
        spins++;
        spinWindow(rng, reelsFG, w);

        // 當轉派彩（FG 賠率表）×線注
        double win = evalAllLines(w, &payFG) * betPerLine;
        base += win;

        // 3+S 再觸發 +5 轉
        if (countScatter(w) >= 3)
        {
            queue += 5;
            retri++;
        }

        // 5 轉為一批次，統計全空批次
        batchSpin++;
        batchWin += win;
        if (batchSpin == 5)
        {
            totalBatches++;
            if (batchWin == 0.0)
                zeroBatches++;
            batchSpin = 0;
            batchWin = 0.0;
        }
    }
    if (batchSpin > 0)
    {
        totalBatches++;
        if (batchWin == 0.0)
            zeroBatches++;
    }
    return {spins, base, retri, zeroBatches, totalBatches};
}

// ≥1000×獎項分佈細分
static const double HIGH_BIN_EDGES[] = {
    1000, 2000, 3000, 4000, 5000,
    6000, 7000, 8000, 9000, 10000,
    11000, 12000, 13000, 14000, 15000,
    20000, 25000, 30000, 40000};
static const int NUM_HIGH_BINS = sizeof(HIGH_BIN_EDGES) / sizeof(HIGH_BIN_EDGES[0]);

/**************
 * 自訂統計（worker 本地先累計，最後匯總）
 **************/
struct Stats
{
    double mainLineWinSum = 0;    // 主遊戲線獎總和（已乘線注）
    double freeGameWinSum = 0;    // 所有 FG 派彩（已乘倍率）
    long long triggerCount = 0;   // 觸發 FG 次數（MG→FG）
    long long retriggerCount = 0; // 再觸發次數（FG 內獲得 3+S）
    long long totalFGSpins = 0;   // FG 實際總轉數
    double maxSingleSpin = 0;     // 單把最高贏分（MG+FG）
    long long deadSpins = 0;      // 無線獎且未觸發 FG 的轉數

    // 依最初觸發倍率分類
    long long trigX10 = 0, trigX50 = 0, trigX100 = 0;

    // 大獎分層（以單把贏分/押注倍率）
    long long bigWins = 0, megaWins = 0, superWins = 0, holyWins = 0, jumboWins = 0, jojoWins = 0;
    long long hiWinBins[NUM_HIGH_BINS] = {};
    // FG 5 轉批次全空統計
    long long fgZeroBatches = 0, fgTotalBatches = 0;

    // per-spin RTP 統計（用於變異/信賴區間）
    double rtpSum = 0, rtpSumSq = 0;
    long long nSpins = 0;
};

/**************
 * 進度心跳（每秒報告；主迴圈每 4096 轉才 atomic 累加）
 **************/
static atomic<long long> spinsDone{0};

static string everyStr(long long totalSpins, long long count)
{
    if (count <= 0)
        return "（—）";
    ostringstream oss;
    oss << "（約每 " << llround((double)totalSpins / (double)count) << " 轉一次）";
    return oss.str();
}

// 啟動心跳：回傳一個 lambda（呼叫即停止心跳）
static function<void()> startProgress(long long total)
{
    auto start = chrono::steady_clock::now();
    auto *stopFlag = new atomic<bool>(false);
    // [Lambda #1]：背景心跳執行緒要跑的函式（放進 std::thread 裡）（以 [=] 值捕捉 start/stopFlag/total）
    thread t([=]()
             {
        while (!stopFlag->load(memory_order_relaxed)) {
            this_thread::sleep_for(chrono::seconds(1)); // 每秒回報
            long long done = spinsDone.load();
            double elapsed = chrono::duration<double>(chrono::steady_clock::now() - start).count();
            double speed = done / max(1e-9, elapsed);
            double eta = (total - done) / max(1e-9, speed);
            std::fprintf(stderr, "[PROGRESS] %lld/%lld (%.2f%%) | %.0f spins/s | ETA %.0fs\n",
                (long long)done, (long long)total,
                100.0 * double(done) / double(total),
                speed, eta);
        } });
    t.detach();
    // [Lambda #2]：回傳給呼叫端的「停止心跳」函式
    return [stopFlag]()
    {
        stopFlag->store(true); // 通知 #1 的 while 退出
        delete stopFlag;
    };
}

/**************
 * Worker：負責跑自己份內的轉數（本地統計 → 結束時寫回）
 **************/
static void worker(int /*id*/, long long spins, Stats *out, uint64_t seed)
{
    mt19937_64 rng(seed);
    Window5x3 w{};
    Stats local{};
    const double perSpinBet = (double)numLines * betPerLine;

    const long long bump = 4096; // 降低 atomic 次數
    long long bumpCnt = 0;

    for (long long i = 0; i < spins; i++)
    {
        //  主遊戲轉窗 + 線獎（未觸發 FG 時也可能有線獎）
        spinWindow(rng, reelsMG, &w);
        double mgLine = evalAllLines(&w, &payMG) * betPerLine;
        double spinTotal = mgLine;

        //  觸發 FG？（3+S）
        int s = countScatter(&w);
        if (s >= 3)
        {
            local.triggerCount++;
            double mul = fgMulByScatter(s);
            if (s >= 5)
                local.trigX100++;
            else if (s == 4)
                local.trigX50++;
            else
                local.trigX10++;

            //  跑完整串 FG（換 FG 輪帶/賠率），回來加總
            auto [fgSp, fgBase, retri, zeroB, totalB] = playFG(rng, &w);
            local.totalFGSpins += fgSp;
            local.retriggerCount += retri;
            local.fgZeroBatches += zeroB;
            local.fgTotalBatches += totalB;

            double fgWin = fgBase * mul;
            local.freeGameWinSum += fgWin;
            spinTotal += fgWin;
        }
        else if (mgLine == 0.0)
        {
            local.deadSpins++; // MG 無線獎且沒進 FG
        }

        //  峰值 & 分層
        local.mainLineWinSum += mgLine;
        if (spinTotal > local.maxSingleSpin)
            local.maxSingleSpin = spinTotal;

        double ratio = spinTotal / perSpinBet; // 單把贏分/押注 倍率
        if (ratio >= 1000)
            local.jojoWins++;
        else if (ratio >= 500)
            local.jumboWins++;
        else if (ratio >= 300)
            local.holyWins++;
        else if (ratio >= 100)
            local.superWins++;
        else if (ratio >= 60)
            local.megaWins++;
        else if (ratio >= 20)
            local.bigWins++;

        // x1000 以上再分層
        if (ratio >= HIGH_BIN_EDGES[0])
        {
            // 從最大門檻往回找，找到第一個符合 ratio >= edge 的 bin
            for (int bi = NUM_HIGH_BINS - 1; bi >= 0; --bi)
            {
                if (ratio >= HIGH_BIN_EDGES[bi])
                {
                    local.hiWinBins[bi]++;
                    break;
                }
            }
        }

        //  per-spin RTP 統計
        local.rtpSum += ratio;
        local.rtpSumSq += ratio * ratio;
        local.nSpins++;

        //  進度累加（每 4096 轉一次）
        if (++bumpCnt == bump)
        {
            spinsDone.fetch_add(bump, memory_order_relaxed);
            bumpCnt = 0;
        }
    }
    if (bumpCnt > 0)
        spinsDone.fetch_add(bumpCnt, memory_order_relaxed);

    *out = local; // 將本地統計回寫
}

/**************
 * 主程式：初始化 → 併發跑轉 → 彙總輸出
 **************/
int main()
{
#ifdef _WIN32
    // 主控台改用 UTF-8，避免 中文/符號 亂碼（與 /utf-8 編譯搭配）
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // 啟動：載入輪帶（字串轉成符號碼）
    reelsMG = packReels(reelsMGstr);
    reelsFG = packReels(reelsFGstr);

    // 進度心跳（每秒報告）
    if (numWorkers <= 0)
        numWorkers = 1;
    auto stopHb = startProgress(numSpins);

    const double totalBet = (double)numSpins * (double)numLines * betPerLine;
    const double perSpinBet = (double)numLines * betPerLine;

    // 依 worker 平分轉數（前 rem 個多 1 轉）
    vector<thread> threads;
    vector<Stats> stats(numWorkers);
    long long chunk = numSpins / numWorkers;
    long long rem = numSpins % numWorkers;

    uint64_t baseSeed = (uint64_t)chrono::high_resolution_clock::now().time_since_epoch().count();

    for (int i = 0; i < numWorkers; i++)
    {
        long long spins = chunk + ((long long)i < rem ? 1 : 0);
        threads.emplace_back([i, spins, &stats, baseSeed]()
                             { worker(i, spins, &stats[i], baseSeed + (uint64_t)i * 1337ULL); });
    }
    for (auto &th : threads)
        th.join();

    // 停心跳
    stopHb();

    // 匯總所有 worker 的統計
    Stats total{};
    for (int i = 0; i < numWorkers; i++)
    {
        const auto &s = stats[i];
        total.mainLineWinSum += s.mainLineWinSum;
        total.freeGameWinSum += s.freeGameWinSum;
        total.triggerCount += s.triggerCount;
        total.retriggerCount += s.retriggerCount;
        total.totalFGSpins += s.totalFGSpins;
        total.maxSingleSpin = max(total.maxSingleSpin, s.maxSingleSpin);
        total.deadSpins += s.deadSpins;

        total.trigX10 += s.trigX10;
        total.trigX50 += s.trigX50;
        total.trigX100 += s.trigX100;

        total.bigWins += s.bigWins;
        total.megaWins += s.megaWins;
        total.superWins += s.superWins;
        total.holyWins += s.holyWins;
        total.jumboWins += s.jumboWins;
        total.jojoWins += s.jojoWins;
        for (int bi = 0; bi < NUM_HIGH_BINS; ++bi)
        {
            total.hiWinBins[bi] += s.hiWinBins[bi];
        }

        total.fgZeroBatches += s.fgZeroBatches;
        total.fgTotalBatches += s.fgTotalBatches;

        total.rtpSum += s.rtpSum;
        total.rtpSumSq += s.rtpSumSq;
        total.nSpins += s.nSpins;
    }

    // ===== 統計輸出 =====
    double totalWin = total.mainLineWinSum + total.freeGameWinSum;
    double rtpMG = total.mainLineWinSum / totalBet;
    double rtpFG = total.freeGameWinSum / totalBet;
    double rtpTotal = totalWin / totalBet;

    cout.setf(std::ios::fixed);
    cout << setprecision(6);
    cout << "=== Monte Carlo | workers=" << numWorkers
         << " | spins=" << numSpins
         << " | lines=" << numLines
         << " | bet/line=" << setprecision(2) << betPerLine << " ===\n";
    cout << setprecision(2);
    cout << "總成本 (Total Bet)                    : " << totalBet << "\n";
    cout << "總贏分 (Total Win)                    : " << totalWin << "\n";
    cout << "最高單把贏分                           : " << total.maxSingleSpin
         << " (x" << (total.maxSingleSpin / perSpinBet) << ")\n";
    cout << setprecision(6);
    cout << "主遊戲 RTP                            : " << rtpMG << "\n";
    cout << "免費遊戲 RTP                          : " << rtpFG << "\n";
    cout << "總 RTP                                : " << rtpTotal << "\n";

    // 觸發與再觸發
    cout << "免費遊戲觸發次數                      : " << total.triggerCount
         << " (觸發率 " << (double)total.triggerCount / (double)numSpins << ") "
         << everyStr(numSpins, total.triggerCount) << "\n";
    cout << "  └×10  次數 (3S)                     : " << total.trigX10
         << " " << everyStr(numSpins, total.trigX10) << "\n";
    cout << "  └×50  次數 (4S)                     : " << total.trigX50
         << " " << everyStr(numSpins, total.trigX50) << "\n";
    cout << "  └×100 次數 (5S)                     : " << total.trigX100
         << " " << everyStr(numSpins, total.trigX100) << "\n";

    double retriRate = 0.0;
    if (total.triggerCount > 0)
    {
        retriRate = (double)total.retriggerCount / (double)total.totalFGSpins;
    }
    cout << "免費遊戲再觸發次數                    : " << total.retriggerCount
         << "（再觸發率 " << retriRate << "）\n";

    if (total.triggerCount > 0)
    {
        cout.setf(std::ios::fixed);
        cout << setprecision(3);
        cout << "每次免費遊戲平均場次（5轉為一場起始）: "
             << (double)total.totalFGSpins / (double)total.triggerCount << "\n";
        cout << setprecision(6);
    }
    // FG 5 轉批次全空
    if (total.fgTotalBatches > 0)
    {
        cout << "FG 5轉批次『完全無贏分』             : "
             << total.fgZeroBatches << " / " << total.fgTotalBatches
             << " (占比 " << (double)total.fgZeroBatches / (double)total.fgTotalBatches << ")\n";
    }
    else
    {
        cout << "FG 5轉批次『完全無贏分』             : 0 / 0 (占比 0)\n";
    }

    cout << "主遊戲 dead spins（無線獎且未觸發FG）: " << total.deadSpins
         << " (占比 " << (double)total.deadSpins / (double)numSpins << ")\n";

    cout << "\n獎項分佈\n";
    cout << "Big  Win  (≥20×bet)                   : " << total.bigWins
         << " " << everyStr(numSpins, total.bigWins) << "\n";
    cout << "Mega Win  (≥60×bet)                   : " << total.megaWins
         << " " << everyStr(numSpins, total.megaWins) << "\n";
    cout << "Super Win (≥100×bet)                  : " << total.superWins
         << " " << everyStr(numSpins, total.superWins) << "\n";
    cout << "Holy Win (≥300×bet)                   : " << total.holyWins
         << " " << everyStr(numSpins, total.holyWins) << "\n";
    cout << "Jumbo Win (≥500×bet)                  : " << total.jumboWins
         << " " << everyStr(numSpins, total.jumboWins) << "\n";
    cout << "Jojo Win  (≥1000×bet)                 : " << total.jojoWins
         << " " << everyStr(numSpins, total.jojoWins) << "\n";

    cout << "\n≥1000倍大獎細分\n";
    for (int bi = 0; bi < NUM_HIGH_BINS; ++bi)
    {
        long long cnt = total.hiWinBins[bi];
        int edge = static_cast<int>(HIGH_BIN_EDGES[bi]);
        cout << "≥" << setw(5) << edge << "×bet    : "
             << cnt << ' ' << everyStr(numSpins, cnt) << '\n';
    }

    // 統計驗證（per-spin RTP 的均值/方差/95% CI）
    double n = (double)total.nSpins;
    double mean = total.rtpSum / n;
    double meanSq = total.rtpSumSq / n;
    double variance = meanSq - mean * mean;
    if (variance < 0)
        variance = 0;
    double se = sqrt(variance / n);
    double lo = mean - 1.96 * se, hi = mean + 1.96 * se;

    cout << "\n=== 統計推論（per-spin RTP） ===\n";
    cout.setf(std::ios::fixed);
    cout << setprecision(0);
    cout << "樣本數 n                              : " << n << "\n";
    cout << setprecision(6);
    cout << "樣本均值 mean(RTP)                    : " << mean << "\n";
    cout << "樣本方差 var(RTP)                     : " << variance << "\n";
    cout << "標準誤差 SE                           : " << se << "\n";
    cout << "95% 信賴區間                         : [" << lo << ", " << hi << "]\n";

    if (excelRTP >= 0)
    {
        double z = (excelRTP - mean) / (se > 0 ? se : 1e-12);
        bool inCI = (excelRTP >= lo && excelRTP <= hi);
        cout << "Excel RTP                             : " << excelRTP << "\n";
        cout.setf(std::ios::fixed);
        cout << setprecision(2);
        cout << "Excel 與樣本均值差的 z 分數           : " << z << "\n";
        cout << setprecision(6);
        cout << (inCI ? "結論：Excel 值落在本次 95% CI 之內（可視為誤差內）。\n"
                      : "結論：Excel 值不在本次 95% CI 之內（建議檢查）。\n");
    }
    return 0;
}
