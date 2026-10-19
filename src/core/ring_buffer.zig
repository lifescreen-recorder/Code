//! Lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer.
//! Designed for zero-latency frame passing between Capture and Encoder threads.
//! Capture thread NEVER blocks on push: if the buffer is full, it drops the frame and counts it.
const std = @import("std");

pub fn SpscRingBuffer(comptime T: type, comptime Capacity: usize) type {
    comptime {
        if (Capacity == 0 or (Capacity & (Capacity - 1)) != 0) {
            @compileError("Capacity must be a power of two");
        }
    }

    return struct {
        const Self = @This();
        const mask = Capacity - 1;

        buffer: [Capacity]T = undefined,
        // Separate cache lines to prevent false sharing between producer and consumer
        head: std.atomic.Value(usize) align(std.atomic.cache_line) = .init(0),
        tail: std.atomic.Value(usize) align(std.atomic.cache_line) = .init(0),
        dropped_count: std.atomic.Value(u64) align(std.atomic.cache_line) = .init(0),

        pub fn init() Self {
            return .{};
        }

        pub fn capacity(_: *const Self) usize {
            return Capacity;
        }

        /// Returns number of items currently available to read
        pub fn available(self: *const Self) usize {
            const h = self.head.load(.acquire);
            const t = self.tail.load(.acquire);
            return t -% h;
        }

        /// Pushes an item into the ring buffer.
        /// If buffer is full, NEVER blocks: increments dropped counter and returns false.
        pub fn tryPush(self: *Self, item: T) bool {
            const t = self.tail.load(.monotonic);
            const h = self.head.load(.acquire);

            if ((t -% h) >= Capacity) {
                // Buffer is full! Count the drop and return false immediately.
                _ = self.dropped_count.fetchAdd(1, .monotonic);
                return false;
            }

            self.buffer[t & mask] = item;
            self.tail.store(t +% 1, .release);
            return true;
        }

        /// Pops an item from the ring buffer.
        /// Returns null if buffer is empty.
        pub fn tryPop(self: *Self) ?T {
            const h = self.head.load(.monotonic);
            const t = self.tail.load(.acquire);

            if (h == t) {
                return null;
            }

            const item = self.buffer[h & mask];
            self.head.store(h +% 1, .release);
            return item;
        }

        /// Resets the ring buffer indices
        pub fn reset(self: *Self) void {
            self.head.store(0, .monotonic);
            self.tail.store(0, .monotonic);
            self.dropped_count.store(0, .monotonic);
        }

        pub fn getDroppedCount(self: *const Self) u64 {
            return self.dropped_count.load(.monotonic);
        }
    };
}

test "SpscRingBuffer basic push and pop" {
    var rb = SpscRingBuffer(u32, 4).init();
    try std.testing.expectEqual(@as(usize, 0), rb.available());

    try std.testing.expect(rb.tryPush(10));
    try std.testing.expect(rb.tryPush(20));
    try std.testing.expect(rb.tryPush(30));
    try std.testing.expectEqual(@as(usize, 3), rb.available());

    try std.testing.expectEqual(@as(?u32, 10), rb.tryPop());
    try std.testing.expectEqual(@as(?u32, 20), rb.tryPop());
    try std.testing.expectEqual(@as(?u32, 30), rb.tryPop());
    try std.testing.expectEqual(@as(?u32, null), rb.tryPop());
    try std.testing.expectEqual(@as(usize, 0), rb.available());
}

test "SpscRingBuffer drops frames when full and does not block" {
    var rb = SpscRingBuffer(u32, 4).init();

    try std.testing.expect(rb.tryPush(1));
    try std.testing.expect(rb.tryPush(2));
    try std.testing.expect(rb.tryPush(3));
    try std.testing.expect(rb.tryPush(4));

    try std.testing.expect(!rb.tryPush(5));
    try std.testing.expect(!rb.tryPush(6));
    try std.testing.expectEqual(@as(u64, 2), rb.getDroppedCount());

    try std.testing.expectEqual(@as(?u32, 1), rb.tryPop());
    try std.testing.expect(rb.tryPush(7));
    try std.testing.expectEqual(@as(u64, 2), rb.getDroppedCount());
}

test "SpscRingBuffer multithreaded producer-consumer" {
    const BufferType = SpscRingBuffer(u64, 64);
    var rb = BufferType.init();

    const total_items: u64 = 10_000;

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
