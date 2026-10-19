// fix_probe.cpp — 为固化「环境阈值 / 瞬态灵敏度」定值
//
// 样本来自 pick_samples.js 的三维分层（质心 × 动态 × 时长），覆盖 samples/wem
// 特征库 62 个非空格中的全部，无缺口。素材名是哈希（pak 索引 AES 加密），
// 无法按名字区分枪声/脚步，只能按频谱形态分层。
//
// 要回答两个问题：
//   Q1 环境阈值该定多少？
//      判据：cut = (edb - thr)*0.5，edb = 频段包络 dBFS。阈值太高会削到脚步
//      主体频段(150-3k)，太低则永不触发。对每个 thr 统计：
//        - 各层样本被触发的比例（触发率）
//        - 被触发时 600-3k 段（脚步主体）被误压的量
//      目标：thr 要落在「能压住低频隆隆、且几乎不碰 600-3k」的档位。
//
//   Q2 瞬态灵敏度该定多少？
//      判据：trSm = clamp((fast-slow - bias)/12, 0, 1)，bias = 6-6*sens
//      对每个 bias 统计：低频隆隆层的触发率 vs 中频瞬态层(脚步/枪)的触发率
//      目标：选择性 = 中频瞬态触发率 / 低频触发率 最大
//
// 用法: fix_probe <file.wav>[|kind]...
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include "dsp.h"

using namespace sl;

static float g_fs = 48000.f;

static bool loadWav(const char* path, std::vector<float>& L, std::vector<float>& R) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);
    if (sz < 44) return false;
    size_t off = 12; int ch = 2, bits = 16; long sr = 48000;
    while (off + 8 <= (size_t)sz) {
        if (off + 8 > (size_t)sz) break;
        uint32_t id = (uint32_t)buf[off] | ((uint32_t)buf[off+1]<<8) |
                      ((uint32_t)buf[off+2]<<16) | ((uint32_t)buf[off+3]<<24);
        uint32_t csz = (uint32_t)buf[off+4] | ((uint32_t)buf[off+5]<<8) |
                       ((uint32_t)buf[off+6]<<16) | ((uint32_t)buf[off+7]<<24);
        if (off + 8 + csz > (size_t)sz) break;
        const unsigned char* p = buf.data() + off + 8;
        if (id == 0x20746D66 && csz >= 16) {
            ch = p[2] | (p[3]<<8);
            sr = (long)p[4] | ((long)p[5]<<8) | ((long)p[6]<<16) | ((long)p[7]<<24);
            bits = p[14] | (p[15]<<8);
        } else if (id == 0x61746164) {
            int bps = bits/8;
            if (ch < 1 || bps < 1 || csz < 8) { off += 8+csz+(csz&1); continue; }
            int frames = (int)(csz/(size_t)(ch*bps));
            if (frames <= 0 || (size_t)frames*ch*bps > csz) { off += 8+csz+(csz&1); continue; }
            L.resize((size_t)frames); R.resize((size_t)frames);
            for (int i = 0; i < frames; ++i) {
                const unsigned char* q = p + (size_t)i*ch*bps;
                float a, b;
                if (bits == 32) { int32_t ia, ib; memcpy(&ia,q,4); memcpy(&ib,q+4,4);
                                  a = ia/2147483648.0f; b = ib/2147483648.0f; }
                else             { int16_t ia, ib; memcpy(&ia,q,2); memcpy(&ib,q+2,2);
                                  a = ia/32768.0f; b = ib/32768.0f; }
                L[(size_t)i]=a; R[(size_t)i]= ch>1?b:a;
            }
            g_fs = (float)sr;
            return !L.empty();
        }
        off += 8 + csz + (csz & 1);
    }
    return false;
}

struct Stat {
    float edb[4] = {0,0,0,0};        // 各频段包络 dB 的中位数
    float tdbP90 = 0;                // (fast-slow) dB 的 90 分位
    float tdbP50 = 0;
    float tdbMax = -200.f;
};

static Stat analyse(const std::vector<float>& L, const std::vector<float>& R) {
    const float fs = g_fs;
    LR4 lp150,hp150,lp600,hp600,lp3k,hp3k;
    lp150.setLP(fs,150.f); hp150.setHP(fs,150.f);
    lp600.setLP(fs,600.f); hp600.setHP(fs,600.f);
    lp3k.setLP(fs,3000.f); hp3k.setHP(fs,3000.f);
    Biquad ap600,ap3k,ap3kb;
    ap600.setAP(fs,600.f,0.70710678f);
    ap3k.setAP(fs,3000.f,0.70710678f); ap3kb.setAP(fs,3000.f,0.70710678f);
    Env envB[4], fw, sw;
    for (int b=0;b<4;++b) envB[b].set(fs,5.f,120.f);
    fw.set(fs,3.f,3.f); sw.set(fs,150.f,150.f);

    std::vector<float> e[4], td;
    const int n0=(int)L.size(), kCB=32;
    for (int base=0; base+kCB<=n0; base+=kCB) {
        int n=std::min(kCB,n0-base);
        for (int i=0;i<n;++i) {
            float l=L[base+i], r=R[base+i];
            float h1l=hp150.hp(l), a0l=lp150.lp(l);
            float h2l=hp600.hp(h1l), b1l=lp600.lp(h1l);
            a0l=ap3k.proc(ap600.proc(a0l));
            b1l=ap3kb.proc(b1l);
            float b0l=a0l, b1x=b1l;
            float b2l=lp3k.lp(h2l), b3l=hp3k.hp(h2l);
            float h1r=hp150.hp(r), a0r=lp150.lp(r);
            float h2r=hp600.hp(h1r), b1r=lp600.lp(h1r);
            float b0r=ap3k.proc(ap600.proc(a0r));
            float b1rx=ap3kb.proc(b1r);
            float b2r=lp3k.lp(h2r), b3r=hp3k.hp(h2r);
            envB[0].proc(0.5f*(b0l+b0r)); envB[1].proc(0.5f*(b1x+b1rx));
            envB[2].proc(0.5f*(b2l+b2r)); envB[3].proc(0.5f*(b3l+b3r));
            float m=0.5f*(l+r); fw.proc(m); sw.proc(m);
        }
        for (int b=0;b<4;++b) e[b].push_back(linToDb(envB[b].e));
        td.push_back(linToDb(fw.e)-linToDb(sw.e));
    }
    Stat s;
    for (int b=0;b<4;++b) {
        if (e[b].empty()) continue;
        std::sort(e[b].begin(), e[b].end());
        s.edb[b] = e[b][e[b].size()/2];
    }
    if (!td.empty()) {
        std::sort(td.begin(), td.end());
        s.tdbP50 = td[td.size()/2];
        s.tdbP90 = td[(size_t)(td.size()*0.90)];
        s.tdbMax = td.back();
    }
    return s;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { printf("usage: fix_probe <wav>[|kind]...\n"); return 1; }
    struct Job { std::string kind, path; std::vector<float> L,R; };
    std::vector<Job> jobs;
    for (int i=1;i<argc;++i) {
        std::string s=argv[i];
        size_t bar=s.rfind('|');
        Job j;
        if (bar!=std::string::npos) { j.kind=s.substr(bar+1); j.path=s.substr(0,bar); }
        else { j.kind="?"; j.path=s; }
        if (!loadWav(j.path.c_str(), j.L, j.R)) { printf("FAIL %s\n", j.path.c_str()); continue; }
        jobs.push_back(std::move(j));
    }
    printf("loaded %zu files\n\n", jobs.size());

    // ---- 按质心层聚合
    // 层定义：低频隆隆 <250 / 脚步带 250-2000 / 枪声带 2000-8000 / 高亮 >8000
    auto layerOf = [](const std::string& k) {
        if (k.rfind("L1_",0)==0) return 0;
        if (k.rfind("L2_",0)==0) return 1;
        if (k.rfind("L3_",0)==0) return 1;
        if (k.rfind("L4_",0)==0) return 2;
        if (k.rfind("L5_",0)==0) return 3;
        return 1;
    };
    static const char* lname[4] = {"L1 low<250", "L2+L3 step250-2k", "L4 gun2k-5k", "L5 bright>5k"};

    std::map<int, std::vector<Stat>> byLayer;
    for (auto& j : jobs) byLayer[layerOf(j.kind)].push_back(analyse(j.L, j.R));

    printf("=== 各层包络 dBFS 中位数 & 瞬态 (fast-slow) dB ===\n");
    printf("%-16s %4s %8s %8s %8s %8s | %8s %8s %8s\n",
           "层","n","<150","150-600","600-3k",">3k","tdbP50","tdbP90","tdbMax");
    for (int L=0;L<4;++L) {
        auto it=byLayer.find(L); if (it==byLayer.end()||it->second.empty()) continue;
        double m[4]={0,0,0,0}, p50=0,p90=0,pmax=-200; int n=(int)it->second.size();
        for (auto& s : it->second) {
            for (int b=0;b<4;++b) m[b]+=s.edb[b];
            p50+=s.tdbP50; p90+=s.tdbP90; pmax=std::max(pmax,(double)s.tdbMax);
        }
        printf("%-16s %4d %8.1f %8.1f %8.1f %8.1f | %8.1f %8.1f %8.1f\n",
               lname[L], n, m[0]/n, m[1]/n, m[2]/n, m[3]/n, p50/n, p90/n, pmax);
    }

    // ---- Q1: 环境阈值扫描
    printf("\n=== Q1环境阈值扫描 ===\n");
    printf("cut = (edb - thr)*0.5, edb=频段包络中位数。触发 = edb < thr\n");
    printf("%8s", "thr dBFS");
    for (int L=0;L<4;++L) printf(" %14s", lname[L]);
    printf("   (每格 = 该层触发率%%)\n");
    for (int thr=-48; thr<=-12; thr+=4) {
        printf("%8d", thr);
        for (int L=0;L<4;++L) {
            auto it=byLayer.find(L); if (it==byLayer.end()||it->second.empty()) { printf(" %14s","-"); continue; }
            int hit=0; int n=(int)it->second.size();
            for (auto& s : it->second) if (s.edb[0] < (float)thr) ++hit;
            printf(" %13.0f%%", hit*100.0/n);
        }
        printf("\n");
    }

    // ---- Q1b: 对 600-3k（脚步主体）的误伤
    printf("\n=== Q1b 阈值对 600-3k 段的误压 (dB, 负=被压) ===\n");
    printf("%8s", "thr dBFS");
    for (int L=0;L<4;++L) printf(" %14s", lname[L]);
    printf("\n");
    for (int thr=-48; thr<=-12; thr+=4) {
        printf("%8d", thr);
        for (int L=0;L<4;++L) {
            auto it=byLayer.find(L); if (it==byLayer.end()||it->second.empty()) { printf(" %14s","-"); continue; }
            double sum=0; int n=0;
            for (auto& s : it->second) {
                if (s.edb[2] < (float)thr) { sum += (s.edb[2]-thr)*0.5; ++n; }
            }
            printf(" %13.1f%s", n?sum/n:0.0, n?"":"(未触发)");
        }
        printf("\n");
    }

    // ---- Q2: 瞬态灵敏度扫描
    printf("\n=== Q2 瞬态判据 bias扫描 (trSm = (tdb-bias)/12 > 0.5 视为触发) ===\n");
    printf("bias dB 对应 sens = (6-bias)/6\n");
    printf("%8s %8s", "bias", "sens%");
    for (int L=0;L<4;++L) printf(" %12s", lname[L]);
    printf(" %10s %10s\n", "脚步带sel", "枪声带sel");
    for (int bias=0; bias<=12; bias+=2) {
        printf("%8d %8.0f", bias, (6.0-bias)/6.0*100);
        double frac[4]={0,0,0,0}; int cnt[4]={0,0,0,0};
        for (int L=0;L<4;++L) {
            auto it=byLayer.find(L); if (it==byLayer.end()||it->second.empty()) { printf(" %12s","-"); continue; }
            int hit=0; int n=(int)it->second.size();
            for (auto& s : it->second) if (s.tdbP90 > (float)bias+6.0) ++hit;  // p90 超过 bias+6 → 明显触发
            frac[L]=hit*100.0/n; cnt[L]=n;
            printf(" %11.0f%%", frac[L]);
        }
        char sel1[32] = "-", sel2[32] = "-";
        if (cnt[1] && cnt[0] && frac[0] > 0) snprintf(sel1, sizeof(sel1), "%.2f", frac[1]/frac[0]);
        if (cnt[2] && cnt[0] && frac[0] > 0) snprintf(sel2, sizeof(sel2), "%.2f", frac[2]/frac[0]);
        printf(" %10s %10s\n", sel1, sel2);
    }
    return 0;
}