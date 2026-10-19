const std = @import("std");
const lifescreen = @import("lifescreen");
const ring = lifescreen.ring_buffer;

test "ring_buffer: basic SPSC push and pop" {
    var rb = ring.SpscRingBuffer(u32, 4).init();
    try std.testing.expectEqual(@as(usize, 0), rb.available());

    try std.testing.expect(rb.tryPush(10));
    try std.testing.expect(rb.tryPush(20));
    try std.testing.expect(rb.tryPush(30));
    try std.testing.expectEqual(@as(usize, 3), rb.available());

    try std.testing.expectEqual(@as(?u32, 10), rb.tryPop());
    try std.testing.expectEqual(@as(?u32, 20), rb.tryPop());
    try std.testing.expectEqual(@as(?u32, 30), rb.tryPop());
    try std.testing.expectEqual(@as(?u32, null), rb.tryPop());
}

test "ring_buffer: drops frames when full and increments counter" {
    var rb = ring.SpscRingBuffer(u32, 4).init();

    try std.testing.expect(rb.tryPush(1));
    try std.testing.expect(rb.tryPush(2));
    try std.testing.expect(rb.tryPush(3));
    try std.testing.expect(rb.tryPush(4));

    // Next pushes must be dropped
    try std.testing.expect(!rb.tryPush(5));
    try std.testing.expect(!rb.tryPush(6));
    try std.testing.expectEqual(@as(u64, 2), rb.getDroppedCount());

    // Consuming frees slots
    try std.testing.expectEqual(@as(?u32, 1), rb.tryPop());
    try std.testing.expect(rb.tryPush(7));
    try std.testing.expectEqual(@as(u64, 2), rb.getDroppedCount());
}

test "ring_buffer: multi-threaded concurrent producer and consumer" {
    const BufferType = ring.SpscRingBuffer(u64, 128);
    var rb = BufferType.init();

    const total_items: u64 = 20_000;

    const Producer = struct {
        fn run(buf: *BufferType, count: u64) void {
            var i: u64 = 0;
            while (i < count) {
                if (buf.tryPush(i)) {
                    i += 1;
                } else {
                    std.Thread.yield() catch {};
                }
            }
        }
    };

    const Consumer = struct {
        fn run(buf: *BufferType, count: u64, sum: *u64) void {
            var received: u64 = 0;
            var acc: u64 = 0;
            while (received < count) {
                if (buf.tryPop()) |val| {
                    acc += val;
                    received += 1;
                } else {
                    std.Thread.yield() catch {};
                }
            }
            sum.* = acc;
        }
    };

    var consumed_sum: u64 = 0;
    const thread_consumer = try std.Thread.spawn(.{}, Consumer.run, .{ &rb, total_items, &consumed_sum });
    const thread_producer = try std.Thread.spawn(.{}, Producer.run, .{ &rb, total_items });

    thread_producer.join();
    thread_consumer.join();

    const expected_sum = (total_items - 1) * total_items / 2;
    try std.testing.expectEqual(expected_sum, consumed_sum);
}
