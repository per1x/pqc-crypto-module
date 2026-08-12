// mlkem_cbd_stream —— PRF 字节流 → 256 个 CBD 系数的流式采样器
//
// FIPS 203 Alg 8（SamplePolyCBD）的硬件形态。上游是 sha3_core 挤出来的
// SHAKE256(σ‖N) 字节流，下游是多项式存储的写口。
//
// 【为什么不像 mlkem_rej_uniform 那样自带存储】
// 拒绝采样的输出速率是不确定的（候选可能被丢弃），所以那边必须先攒满
// 256 个再让上层读。CBD 不拒绝：η=2 时 4 字节恒出 8 个系数，η=3 时
// 3 字节恒出 4 个 —— 系数是**按 0…255 的顺序**依次冒出来的，上层直接
// 一拍写一个进多项式存储就行。自带一块 256×16 的 BRAM 纯属浪费，
// 还多一次 256 拍的搬运。
//
// 【为什么吸收和输出是分开的两段，而不是流水】
// 输入 1 字节/周期，输出 1 系数/周期，而 η=2 的换算是 4 字节 → 8 系数 ——
// 输出比输入快一倍，流水起来反而要在中间加一个变速缓冲。这里的做法是
// 攒够一组（4 或 3 字节）就把 in_ready 拉低，把这一组的 8 或 4 个系数
// 挤完再收下一组。一个多项式的总开销：
//   η=2：128 字节 + 256 系数 = 384 周期
//   η=3：192 字节 + 256 系数 = 448 周期
// 这个数量级完全被 SHAKE 的置换本身盖住（每 136 字节要 24 个周期置换），
// 不值得为它加缓冲。
//
// 【位序】
// rand_in 的第 0 字节是**最低地址**那一字节（小端），与 ref_model 里
// int.from_bytes(buf[off:off+n], "little") 一致。cbd2 / cbd3 两个组合模块
// 已经把位并行的汉明重量算法做好了，这里只负责喂字节、取系数。
`default_nettype none

module mlkem_cbd_stream (
    input  wire        clk,
    input  wire        rst_n,

    // η 选择：0 → η=2（ML-KEM-768/1024 的 e、以及 512 的 e2）
    //          1 → η=3（ML-KEM-512 的 s/e）
    // 只在 start 那一拍被锁存，采样途中改它不生效。
    input  wire        eta3,

    input  wire        start,      // 脉冲：清计数，开始采样
    output reg         done,       // 电平：置位后保持到下一次 start

    // ---- PRF 字节入口 ----
    input  wire        in_valid,
    output wire        in_ready,
    input  wire [7:0]  in_data,

    // ---- 系数出口（按 0…255 顺序）----
    output wire        out_valid,
    input  wire        out_ready,
    output wire signed [15:0] out_coeff,

    output reg  [8:0]  count       // 已经吐出去的系数个数
);
    reg        eta3_r;
    reg        busy;

    // 攒字节：最多 4 个
    reg [31:0] buf_r;
    reg [2:0]  bcnt;

    // 吐系数：最多 8 个，低位在前
    reg [127:0] sh;
    reg [3:0]   ocnt;

    wire [2:0] group_bytes  = eta3_r ? 3'd3 : 3'd4;
    wire [3:0] group_coeffs = eta3_r ? 4'd4 : 4'd8;

    // 吐完一组之前不收新字节；采样结束（!busy）之后也不收 ——
    // 否则 sha3_core 会被白白抽走一批字节，下一个多项式的采样就错位了。
    assign in_ready  = busy && (ocnt == 4'd0);
    assign out_valid = busy && (ocnt != 4'd0);
    assign out_coeff = sh[15:0];

    wire byte_fire  = in_valid  && in_ready;
    wire coeff_fire = out_valid && out_ready;

    // 这一拍收下的字节让这一组凑满了吗
    wire group_full = byte_fire && (bcnt + 3'd1 == group_bytes);

    // group_full 那一拍 buf_r 还没更新，cbd2/cbd3 要看的是「带上这个字节」的值
    wire [31:0] buf_next = (buf_r & ~(32'hFF << {bcnt, 3'd0}))
                         | ({24'd0, in_data} << {bcnt, 3'd0});

    // ⚠️ 两个组合模块吃的是 **buf_next** 而不是 buf_r：
    // 让这一组凑满的那个字节是在 byte_fire 当拍到的，buf_r 要等到下一拍才更新。
    // 接 buf_r 就会漏掉最后一个字节 —— 每组的最后一个字节恒为 0。
    // buf_next 在 !byte_fire 时等于 buf_r，此时结果不被采样，无所谓。
    wire [127:0] c2;
    wire [63:0]  c3;
    mlkem_cbd2 u_cbd2 (.rand_in(buf_next),       .coeffs(c2));
    mlkem_cbd3 u_cbd3 (.rand_in(buf_next[23:0]), .coeffs(c3));

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            done   <= 1'b0;
            busy   <= 1'b0;
            eta3_r <= 1'b0;
            buf_r  <= 32'd0;
            bcnt   <= 3'd0;
            sh     <= 128'd0;
            ocnt   <= 4'd0;
            count  <= 9'd0;
        end else if (start) begin
            // done 保持到下一次 start —— 与仓库里其它核同一个契约
            done   <= 1'b0;
            busy   <= 1'b1;
            eta3_r <= eta3;
            buf_r  <= 32'd0;
            bcnt   <= 3'd0;
            sh     <= 128'd0;
            ocnt   <= 4'd0;
            count  <= 9'd0;
        end else begin
            if (byte_fire) begin
                buf_r <= buf_next;
                if (group_full) begin
                    bcnt <= 3'd0;
                    ocnt <= group_coeffs;
                end else begin
                    bcnt <= bcnt + 3'd1;
                end
            end

            if (group_full) begin
                sh <= eta3_r ? {64'd0, c3} : c2;
            end

            if (coeff_fire) begin
                sh    <= {16'd0, sh[127:16]};
                ocnt  <= ocnt - 4'd1;
                count <= count + 9'd1;
                if (count == 9'd255) begin
                    busy <= 1'b0;
                    done <= 1'b1;
                end
            end
        end
    end

`ifndef SYNTHESIS
    // 采样期间改 η 是个上层的 bug，静默按旧值跑会很难查
    always @(posedge clk) begin
        if (rst_n && busy && !start && (eta3 !== eta3_r)) begin
            $display("[%0t] mlkem_cbd_stream 断言失败：采样途中改了 eta3", $time);
            $stop;
        end
    end
`endif
endmodule

`default_nettype wire
