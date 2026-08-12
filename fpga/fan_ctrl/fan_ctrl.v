// fan_ctrl —— 按结温调速的风扇控制器（AXU3EGB，PWM 出到 AA11）
//
// 出厂设计把风扇直接拉成常转满速，噪音很大。这个模块读 ZynqMP 的片上温度，
// 按档位给 PWM，低温安静、高温跟上。
//
// ============================================================================
// 【为什么温度必须从 PL 的 SYSMONE4 读，而不是让 Linux 读了写寄存器】
// ============================================================================
// 风扇是**散热**，不是外设：它必须在没有 Linux 的时候也工作 —— 开机那几十秒、
// U-Boot 里停着的时候、内核挂死的时候、以及任何"软件本该更新占空比却没更新"
// 的时刻。把温度源放在 PS 侧，等于让散热依赖软件活着。
//
// 所以温度直接从 PL 的系统监测单元（UltraScale+ 的 SYSMONE4）经 DRP 读，
// 整条链路只依赖 PL 时钟。AXI 那一路（fan_ctrl_axi）**只是观测口**，
// 拔掉它风扇照样正常工作。
//
// ============================================================================
// 【AA11 是低有效：低=转，高=停】
// ============================================================================
// 所以"占空比 D"对应的是**引脚为低的时间占 D**。这一点写反了的后果是
// 温度越高越安静 —— 而且在实验室里未必立刻看出来，等看出来时芯片已经烤过了。
// 下面的 `fan_pin = ~pwm_on` 是唯一一处反相，别在别处再反一次。
//
// ============================================================================
// 【阈值用 ADC 码，不用摄氏度】
// ============================================================================
// UltraScale+ 的换算是 T(°C) = code/65536 × 502.9098127 − 273.8195117。
// 在 RTL 里做这个换算要除法和小数；反过来把每个档位的摄氏度**在生成时**
// 反算成整数 ADC 码，RTL 里就只剩比较大小。
//     code = (T + 273.8195117) × 65536 / 502.9098127
// 下面每个常数后面都标了它对应的摄氏度，改的时候按注释改，别直接改数字。
//
// ============================================================================
// 【三道安全，方向永远是「多吹」】
// ============================================================================
//  ① 最低档不是 0% 而是 DUTY_MIN —— ZU3EG 满载发热大，把风扇停死不是省电
//     而是赌运气。这一条是**故意不做到最静**的。
//  ② 超过 OT_ON（80°C）强制 100%，直到降回 OT_OFF（74°C）才解除。
//  ③ **SYSMON 迟迟读不到有效数据就强制 100%**。温度未知时唯一安全的假设
//     是"可能很热"。上电后 SYSMON 需要若干个采样周期才有第一个有效值，
//     这段时间里风扇是满速的 —— 听起来像出厂状态，几秒后自己降下来。
//  ④ **读数长时间一个比特都不变，也强制 100%。**
//     这一条是上板之后补的，因为第 ③ 条挡不住真实发生的那种坏法：
//     SYSMON 配置错了的时候，DRP **照样应答**，寄存器里**照样有一个看着
//     很合理的温度**（32.5°C），只是 ADC 根本没在转换。于是"读不到"永远
//     不成立，风扇心安理得停在最低档，而没有任何人会发现。
//     真实的结温永远在抖 —— 同期 PS 侧 AMS 的读数在 24.9~29.1°C 之间动，
//     而我这条链路五分钟六十次采样一个比特没变。所以"完全不变"本身就是
//     故障信号。误判的方向是风扇多吹，可以接受。
`default_nettype none

module fan_ctrl #(
    // PWM 周期：25 kHz 左右，避开人耳敏感区。75 MHz / 3000 = 25 kHz
    parameter integer PWM_PERIOD = 3000,
    // 多久没拿到有效温度就当故障（75 MHz 下约 0.9 秒）
    parameter integer STALE_LIMIT = 64_000_000,
    // 连续多少次采样读数**完全相同**就当传感器卡死（见文件头第 ④ 条）。
    // 单位是"采样次数"不是时钟拍数，这样换时钟频率不用重算 —— 采样间隔由
    // fan_sysmon 的 PERIOD 定（1 ms），30000 次 ≈ 30 秒。
    parameter integer STUCK_LIMIT = 30_000
) (
    input  wire        clk,
    input  wire        rst_n,

    // ---- 温度输入（来自 fan_sysmon）----
    input  wire [15:0] temp_code,      // SYSMON 的 16 位温度码
    input  wire        temp_valid,     // 每次更新拉一拍

    // ---- 观测/覆盖（来自 AXI，可选）----
    input  wire        ovr_en,         // 1 = 用 ovr_duty 代替自动档位
    input  wire [7:0]  ovr_duty,       // 0..100
    output wire [15:0] cur_temp,       // 最近一次温度码
    output wire [7:0]  cur_duty,       // 当前占空比（百分比）
    output wire [2:0]  cur_step,       // 当前档位 0..5
    output wire        forced_full,    // 因高温 / 数据陈旧 / 传感器卡死被强制满速
    output wire        sensor_stuck,   // 读数长时间一个比特不变

    // ---- 出到 AA11 ----
    output wire        fan_pin
);
    // ================= 温度阈值（ADC 码）=================
    // 生成方式见文件头；括号里是对应摄氏度。改档位请改摄氏度再重算。
    localparam [15:0] T1_UP   = 16'd41547;  // 45°C
    localparam [15:0] T1_DN   = 16'd40895;  // 40°C
    localparam [15:0] T2_UP   = 16'd42850;  // 55°C
    localparam [15:0] T2_DN   = 16'd42198;  // 50°C
    localparam [15:0] T3_UP   = 16'd44153;  // 65°C
    localparam [15:0] T3_DN   = 16'd43501;  // 60°C
    localparam [15:0] T4_UP   = 16'd45456;  // 75°C
    localparam [15:0] T4_DN   = 16'd44804;  // 70°C
    localparam [15:0] OT_ON   = 16'd46108;  // 80°C 强制全速
    localparam [15:0] OT_OFF  = 16'd45326;  // 74°C 解除

    // ================= 档位对应的占空比 =================
    // 0 档是**保底转速**不是停转，理由见文件头第 ① 条。
    localparam [7:0] DUTY_MIN = 8'd25;
    localparam [7:0] DUTY_1   = 8'd40;
    localparam [7:0] DUTY_2   = 8'd60;
    localparam [7:0] DUTY_3   = 8'd80;
    localparam [7:0] DUTY_4   = 8'd100;

    // ================= 迟滞档位机 =================
    reg  [2:0]  step;
    reg  [15:0] temp_r;
    reg         ot_latch;
    reg  [25:0] stale;          // 距上次有效温度多久
    wire        stale_bad = (stale >= STALE_LIMIT[25:0]);
    reg  [15:0] same_cnt;       // 连续多少次采样读数完全相同
    wire        stuck_bad = (same_cnt >= STUCK_LIMIT[15:0]);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            step     <= 3'd4;       // 上电先满速，等第一个温度再降 —— 见第 ③ 条
            temp_r   <= 16'd0;
            ot_latch <= 1'b1;
            stale    <= {26{1'b1}};
            same_cnt <= 16'd0;
        end else begin
            if (temp_valid) begin
                temp_r <= temp_code;
                stale  <= 26'd0;

                // 卡死检测：一模一样就累加，变了就清零。到顶不再加，
                // 否则会绕回 0 把故障状态自己"治好"。
                if (temp_code == temp_r) begin
                    if (!stuck_bad) same_cnt <= same_cnt + 16'd1;
                end else begin
                    same_cnt <= 16'd0;
                end

                // 过温锁存：进得早、出得晚
                if (temp_code >= OT_ON)       ot_latch <= 1'b1;
                else if (temp_code <= OT_OFF) ot_latch <= 1'b0;

                // 一次只升/降一档：温度是慢变量，跳档没有意义，
                // 而逐档走能让"档位"这个状态本身有可读性
                if (step < 3'd4 && temp_code >= (step == 3'd0 ? T1_UP :
                                                 step == 3'd1 ? T2_UP :
                                                 step == 3'd2 ? T3_UP : T4_UP))
                    step <= step + 3'd1;
                else if (step > 3'd0 && temp_code <= (step == 3'd1 ? T1_DN :
                                                      step == 3'd2 ? T2_DN :
                                                      step == 3'd3 ? T3_DN : T4_DN))
                    step <= step - 3'd1;
            end else if (!stale_bad) begin
                stale <= stale + 26'd1;
            end
        end
    end

    // ================= 占空比选择 =================
    wire [7:0] auto_duty = (step == 3'd0) ? DUTY_MIN :
                           (step == 3'd1) ? DUTY_1   :
                           (step == 3'd2) ? DUTY_2   :
                           (step == 3'd3) ? DUTY_3   : DUTY_4;

    // 强制满速优先于一切，包括手动覆盖 —— 覆盖是给调试用的，
    // 不能让调试的人把芯片烤了。
    wire force_full = ot_latch || stale_bad || stuck_bad;
    wire [7:0] duty = force_full ? 8'd100
                    : ovr_en     ? ((ovr_duty > 8'd100) ? 8'd100 : ovr_duty)
                                 : auto_duty;

    // ================= PWM =================
    // 阈值 = 周期 × duty / 100。周期是参数、duty 最大 100，
    // 乘法结果最宽 3000×100 = 300000，20 位够。
    reg  [31:0] pwm_cnt;
    wire [31:0] thresh = (PWM_PERIOD * duty) / 32'd100;
    wire        pwm_on = (pwm_cnt < thresh);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)                              pwm_cnt <= 32'd0;
        else if (pwm_cnt + 32'd1 >= PWM_PERIOD)  pwm_cnt <= 32'd0;
        else                                     pwm_cnt <= pwm_cnt + 32'd1;
    end

    // ⚠️ 唯一一处反相：AA11 低=转。别在别处再反。
    assign fan_pin = ~pwm_on;

    assign cur_temp    = temp_r;
    assign cur_duty    = duty;
    assign cur_step    = step;
    assign forced_full  = force_full;
    assign sensor_stuck = stuck_bad;

endmodule

`default_nettype wire
