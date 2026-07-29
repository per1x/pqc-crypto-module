// tb_trng_health —— 仅供仿真的汇总顶层
//
// 两个实例吃同一条样本流：
//   u_main  按 SP 800-90B 现算出来的阈值（RCT C=41，APT W=1024 C=793）
//   u_ctl   阈值调高到本测试的样本数内不可能触发 —— 反证用的空对照
//
// 空对照回答的是"告警到底是不是这两项检测判出来的"：同一条卡死流在 u_main 上
// 必须告警、在 u_ctl 上必须不告警。少了它，一个恒为 1 的告警位也能让所有
// 正向断言通过。
//
// 本文件不参与综合，也不在 hardware/rtl/ 下。
`default_nettype none

module tb_trng_health (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clear,
    input  wire        sample_valid,
    input  wire        sample,

    output wire        rct_alarm,
    output wire        apt_alarm,
    output wire        alarm,
    output wire [15:0] rct_run,
    output wire [15:0] apt_count,
    output wire [15:0] apt_index,

    output wire        ctl_rct_alarm,
    output wire        ctl_apt_alarm
);

    trng_health #(
        .SAMPLE_W(1), .RCT_CUTOFF(41), .APT_WINDOW(1024), .APT_CUTOFF(793)
    ) u_main (
        .clk(clk), .rst_n(rst_n), .clear(clear),
        .sample_valid(sample_valid), .sample(sample),
        .rct_alarm(rct_alarm), .apt_alarm(apt_alarm), .alarm(alarm),
        .rct_run(rct_run), .apt_count(apt_count), .apt_index(apt_index));

    trng_health #(
        .SAMPLE_W(1), .RCT_CUTOFF(60000), .APT_WINDOW(1024), .APT_CUTOFF(1024)
    ) u_ctl (
        .clk(clk), .rst_n(rst_n), .clear(clear),
        .sample_valid(sample_valid), .sample(sample),
        .rct_alarm(ctl_rct_alarm), .apt_alarm(ctl_apt_alarm), .alarm(),
        .rct_run(), .apt_count(), .apt_index());
endmodule

`default_nettype wire
