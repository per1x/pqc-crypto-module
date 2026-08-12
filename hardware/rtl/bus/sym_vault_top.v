// sym_vault_top —— 把密钥仓与对称核接在一起的顶层
//
//   vault_s_axi ──▶ key_vault_axi ──use 口(256 bit)──▶ sym_axi ──▶ AES/SM4/SM3
//   sym_s_axi   ──────────────────────────────────────▶┘
//                                     ▲
//                              tamper 一根线通到两边
//
// 两个 AXI4-Lite 从机各自带防火墙，**没有互联**：真系统里它们挂在 PS 的
// 不同地址上，各自的 XMPU/XPPU 规则也不同（密钥仓通常比算法核管得更严）。
// 在这里合成一个互联反而会把"两段地址、两套策略"这件事糊掉。
//
// ============================================================================
// 【这个顶层要证明的那件事】
// ============================================================================
// 软件的完整流程是：
//
//     ① 往密钥仓的槽 n 写 8 个字、COMMIT     ← 密钥在这条总线上走过一次
//     ② 告诉 sym_axi "用槽 n"（写 KEY_SLOT） ← 只是一个 3 位的槽号
//     ③ LOAD_KEY / BLOCK                     ← 密钥从 use 口直接进算法核
//
// 第 ① 步之后，**密钥在两条总线上都不再有任何可读地址**。
// 第 ③ 步用到的那 256 根线是 PL 内部的导线，不出芯片。
//
// test_sym_vault 里那条 `test_key_never_on_either_bus` 就是把这句话变成
// 一条会失败的检查：装完密钥之后，把**两个从机的整个地址窗口**逐字扫一遍，
// 断言密钥的 8 个字一个都不出现，而与此同时 AES 用同一把密钥算出的密文
// 与 FIPS 197 的官方向量一致 —— 密钥确实到了核里，只是没有一条路能读到它。
`default_nettype none

module sym_vault_top #(
    parameter integer SECURE_ONLY = 1
) (
    input  wire        clk,
    input  wire        rst_n,

    // ---- 篡改检测：一根线同时打到密钥仓与三个算法核 ----
    input  wire        tamper,

    // ---- 密钥仓的 AXI4-Lite 从机 ----
    input  wire [7:0]  vault_awaddr,
    input  wire [2:0]  vault_awprot,
    input  wire        vault_awvalid,
    output wire        vault_awready,
    input  wire [31:0] vault_wdata,
    input  wire [3:0]  vault_wstrb,
    input  wire        vault_wvalid,
    output wire        vault_wready,
    output wire [1:0]  vault_bresp,
    output wire        vault_bvalid,
    input  wire        vault_bready,
    input  wire [7:0]  vault_araddr,
    input  wire [2:0]  vault_arprot,
    input  wire        vault_arvalid,
    output wire        vault_arready,
    output wire [31:0] vault_rdata,
    output wire [1:0]  vault_rresp,
    output wire        vault_rvalid,
    input  wire        vault_rready,

    // ---- 对称核的 AXI4-Lite 从机 ----
    input  wire [7:0]  sym_awaddr,
    input  wire [2:0]  sym_awprot,
    input  wire        sym_awvalid,
    output wire        sym_awready,
    input  wire [31:0] sym_wdata,
    input  wire [3:0]  sym_wstrb,
    input  wire        sym_wvalid,
    output wire        sym_wready,
    output wire [1:0]  sym_bresp,
    output wire        sym_bvalid,
    input  wire        sym_bready,
    input  wire [7:0]  sym_araddr,
    input  wire [2:0]  sym_arprot,
    input  wire        sym_arvalid,
    output wire        sym_arready,
    output wire [31:0] sym_rdata,
    output wire [1:0]  sym_rresp,
    output wire        sym_rvalid,
    input  wire        sym_rready,

    output wire        vault_tampered
);
    // ================= 密钥仓与算法核之间的那 256 根线 =================
    // **它们不接到任何一个 AXI 从机的读通路上。** 这是整个 S6 的落点。
    wire [2:0]   kv_sel;
    wire [255:0] kv_key;
    wire         kv_valid;

    key_vault_axi #(
        .SECURE_ONLY(SECURE_ONLY), .SLOTS(8), .SLOT_BITS(3), .WORDS(8)
    ) u_vault (
        .clk(clk), .rst_n(rst_n),
        .s_axi_awaddr(vault_awaddr), .s_axi_awprot(vault_awprot),
        .s_axi_awvalid(vault_awvalid), .s_axi_awready(vault_awready),
        .s_axi_wdata(vault_wdata), .s_axi_wstrb(vault_wstrb),
        .s_axi_wvalid(vault_wvalid), .s_axi_wready(vault_wready),
        .s_axi_bresp(vault_bresp), .s_axi_bvalid(vault_bvalid),
        .s_axi_bready(vault_bready),
        .s_axi_araddr(vault_araddr), .s_axi_arprot(vault_arprot),
        .s_axi_arvalid(vault_arvalid), .s_axi_arready(vault_arready),
        .s_axi_rdata(vault_rdata), .s_axi_rresp(vault_rresp),
        .s_axi_rvalid(vault_rvalid), .s_axi_rready(vault_rready),
        .tamper(tamper),
        .use_sel(kv_sel), .use_key(kv_key), .use_valid(kv_valid),
        .vault_tampered(vault_tampered));

    sym_axi #(.SECURE_ONLY(SECURE_ONLY)) u_sym (
        .clk(clk), .rst_n(rst_n),
        .s_axi_awaddr(sym_awaddr), .s_axi_awprot(sym_awprot),
        .s_axi_awvalid(sym_awvalid), .s_axi_awready(sym_awready),
        .s_axi_wdata(sym_wdata), .s_axi_wstrb(sym_wstrb),
        .s_axi_wvalid(sym_wvalid), .s_axi_wready(sym_wready),
        .s_axi_bresp(sym_bresp), .s_axi_bvalid(sym_bvalid),
        .s_axi_bready(sym_bready),
        .s_axi_araddr(sym_araddr), .s_axi_arprot(sym_arprot),
        .s_axi_arvalid(sym_arvalid), .s_axi_arready(sym_arready),
        .s_axi_rdata(sym_rdata), .s_axi_rresp(sym_rresp),
        .s_axi_rvalid(sym_rvalid), .s_axi_rready(sym_rready),
        .tamper(tamper),
        .kv_sel(kv_sel), .kv_key(kv_key), .kv_valid(kv_valid));

endmodule

`default_nettype wire
