# Syrebas 复刻核心源码（电路级 TB-303，作为本项目 RP2350 引擎的对齐目标）

来源：https://github.com/holstebroe/Syrebas （MIT / 见其 LICENSE）。
本文件仅作对照参考；官方另有 TB303_EMULATION_REFERENCE.md（权威）与 GUIDE.md。

## 架构总览
- **Oscillator**：负向锯齿 `1-2*phase` → 14kHz 一阶 LPF（磨圆尖）→ 二次弯曲 `x-0.05x²`；
  方波占空比随音高 45%→70%，电平 ±0.75，再经 150Hz 一阶 HPF（tilt/去超低）；滑音 τ=60ms，1-pole，相位不复位。
- **Envelope**：VCF(MEG) attack 3.5ms / decay 200ms–2.5s（`0.2*12.5^d`）/ accent 时 decay 按 accent 旋钮推向 200ms；
  VCA(VEG) attack 3ms / gate-high 慢放 3.5s / gate-low 快泄 16ms；**slide 不重触发**。
  Accent sweep 电容：47k+1µF=47ms 充放、跨音符连续；accent→VCA RC 47k+0.033µF=1.55ms。
- **Filter**：4 级耦合二极管梯形，**4× 过采样 RK2(中点)**；反馈 HPF 150→250Hz；k=res*33；Vt=0.052（2*26mV）；
  ODE: dv_i/dt = w*Vt*[tanh((v_{i-1}-v_i)/Vt) - tanh((v_i-v_{i+1})/Vt)]，dv4/dt=2w*Vt*tanh((v3-v4)/Vt)；
  输入按 0.05V 归一，输出 v4/0.05×0.25 平均降采。
- **SynthEngine（控制电流域）**：cut/envMod 用 `c²` 近似 A 锥；cv_base=3.64385*c²；cv_offset=envMod²*0.80735；
  cv_envmod=(effEnvMod²)*vcfEnv*3.5；accent sweep 按 res 分 `direct=(1-0.7res)*vcfEnv` + `cap=0.7res*accentCap`；
  cutoff=200*2^cv_total*(1-0.15res)；VCA=vcaEnv+accent*accentVca*0.8，非对称 tanh(x*1.1 / x*0.9)，× master。

（完整源码见下文，供逐行移植。）

## Filter (非核心头略)
见 Syrebas src/core/Filter.cpp：solveTridiagonal4 + processSample（4×(K1/K2)）。

## 移植要点（本项目）
- RP2350 路径用本引擎；RP2040 保留旧 Open303 级引擎。
- tanh 用 fastTanh（纯 FPU Padé）。
- 旋钮：Syrebas 假定线性旋钮 0..1 后在引擎内做 c²；本项目电位器为 B 线性，可直接喂线性值，锥度由引擎 c² 完成。
