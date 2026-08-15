// ML-DSA 的通用位解包（mldsa_bitpack 的逆）—— skDecode / ExpandMask 用
//
// ============================================================================
// 【这一层解决什么】
// ============================================================================
// 打包时每个系数 W 位、低位在前、攒够 8 位吐一字节（见 pack.v）。解包要反着来：
// **字节一个个进来，W 位系数一个个出去**，内部一个位累加器。位宽不是 8 的因数
// （3/13/18 位）时每个系数横跨相邻字节，这个累加器把「跨字节」这件麻烦事收在一处。
//
// 与 mldsa_bitpack 完全对偶：
//   · 累加器低位先用（先进的字节在低位，先出的系数取自低 W 位）；
//   · nbits < W 时才吸字节（in_ready），nbits ≥ W 时才吐系数（out_valid）——
//     两者互斥，一拍只做一件事。
//
// ⚠️ 这里只做「位」的搬运，**不含 η−v / 2^(D−1)−v / γ₁−v 那类逆变换**。
// 那些变换是有符号/无符号搬移，放在调用方（sign.v）做，理由同 pack.v：
// 让本模块的正确性只依赖「宽度对不对」一件事。
`default_nettype none

// ⚠️ 位宽 w 是**运行时输入**，理由与 mldsa_bitpack 那边一样（见 pack.v 的说明）：
//    同一个 bitstream 要能运行时选 44/65/87，而 s₁/s₂ 随 η 变、z 随 γ₁ 变。
module mldsa_bitunpack (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clr,           // 每条多项式开始前拉一拍，清空累加器

    input  wire [4:0]  w,             // 每个系数占的位数，运行时给（3…20）
    input  wire [7:0]  in_byte,
    input  wire        in_valid,
    output wire        in_ready,      // 累加器不足 w 位时才收字节

    output wire [19:0] out_val,       // 低 w 位就是下一个系数的原始无符号值
    output wire        out_valid,     // 累加器已够 w 位
    input  wire        out_ready
);
    // 累加器要装得下「吐一个系数前的残留（最多 w−1 位）」+「新吸的一整字节（8 位）」。
    // w 最大 20 ⇒ 19+8 = 27，取 28，与 mldsa_bitpack 对称。
    reg [27:0] acc;
    reg [4:0]  nbits;

    assign out_valid = (nbits >= w);
    assign in_ready  = (nbits <  w);

    // 只取低 w 位。理由与 pack.v 那边一样：查表而不是 (1<<w)−1，
    // 后者是桶形移位器加借位链，会落到关键路径上。
    reg [19:0] w_mask;
    always @(*) begin
        case (w)
            5'd3:  w_mask = 20'h00007;
            5'd4:  w_mask = 20'h0000F;
            5'd6:  w_mask = 20'h0003F;
            5'd10: w_mask = 20'h003FF;
            5'd13: w_mask = 20'h01FFF;
            5'd18: w_mask = 20'h3FFFF;
            default: w_mask = 20'hFFFFF;   // w=20
        endcase
    end
    assign out_val = acc[19:0] & w_mask;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc <= 28'd0;
            nbits <= 5'd0;
        end else if (clr) begin
            acc <= 28'd0;
            nbits <= 5'd0;
        end else if (out_valid && out_ready) begin
            // 吐掉低 w 位，剩下的下移
            acc   <= acc >> w;
            nbits <= nbits - w;
        end else if (in_valid && in_ready) begin
            // 新字节拼到高位侧：低位在前的约定就是这一句（与 bitpack 对偶）
            acc   <= acc | ({20'd0, in_byte} << nbits);
            nbits <= nbits + 5'd8;
        end
    end
endmodule

`default_nettype wire
