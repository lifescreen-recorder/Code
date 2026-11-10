const std = @import("std");
const math = std.math;

pub fn dbToLinear(db: f32) f32 {
    if (db <= -96.0) return 0.0;
    return math.pow(f32, 10.0, db / 20.0);
}

pub fn linearToDb(lin: f32) f32 {
    if (lin <= 1.0e-5) return -100.0;
    return 20.0 * @log10(lin);
}

pub fn timeToCoeff(time_ms: f32, sample_rate: f32) f32 {
    if (time_ms <= 0.01 or sample_rate <= 0.0) return 0.0;
    const samples = (time_ms * 0.001) * sample_rate;
    return @exp(-1.0 / samples);
}

pub const BiquadSection = struct {
    b0: f32 = 1.0,
    b1: f32 = 0.0,
    b2: f32 = 0.0,
    a1: f32 = 0.0,
    a2: f32 = 0.0,
    z1: [2]f32 = .{ 0.0, 0.0 },
    z2: [2]f32 = .{ 0.0, 0.0 },

    pub fn initHighPass(cutoff_hz: f32, q: f32, sample_rate: f32) BiquadSection {
        const sr = if (sample_rate > 1000.0) sample_rate else 48000.0;
        const fc = math.clamp(cutoff_hz, 20.0, sr * 0.45);
        const w0 = 2.0 * math.pi * fc / sr;
        const cos_w0 = @cos(w0);
        const sin_w0 = @sin(w0);
        const alpha = sin_w0 / (2.0 * @max(q, 0.1));

        const a0 = 1.0 + alpha;
        const inv_a0 = 1.0 / a0;
        const half_one_plus_cos = (1.0 + cos_w0) * 0.5;

        return BiquadSection{
            .b0 = half_one_plus_cos * inv_a0,
            .b1 = -(1.0 + cos_w0) * inv_a0,
            .b2 = half_one_plus_cos * inv_a0,
            .a1 = (-2.0 * cos_w0) * inv_a0,
            .a2 = (1.0 - alpha) * inv_a0,
        };
    }

    pub inline fn processSample(self: *BiquadSection, x: f32, ch: usize) f32 {
        const y = self.b0 * x + self.z1[ch];
        self.z1[ch] = self.b1 * x - self.a1 * y + self.z2[ch];
        self.z2[ch] = self.b2 * x - self.a2 * y;
        return y;
    }

    pub fn reset(self: *BiquadSection) void {
        self.z1 = .{ 0.0, 0.0 };
        self.z2 = .{ 0.0, 0.0 };
    }
};

pub const HighPass = struct {
    stage1: BiquadSection,
    stage2: BiquadSection,
    enabled: bool = true,

    pub fn init(sample_rate: f32, cutoff_hz: f32) HighPass {
        return HighPass{
            .stage1 = BiquadSection.initHighPass(cutoff_hz, 0.54119610, sample_rate),
            .stage2 = BiquadSection.initHighPass(cutoff_hz, 1.3065630, sample_rate),
            .enabled = true,
        };
    }

    pub fn processBuffer(self: *HighPass, samples: []f32, channels: u32) void {
        if (!self.enabled or channels == 0) return;
        const ch_count: usize = @min(@as(usize, channels), 2);
        const stride: usize = @intCast(channels);

        var i: usize = 0;
        while (i + stride <= samples.len) : (i += stride) {
            for (0..ch_count) |ch| {
                const s1 = self.stage1.processSample(samples[i + ch], ch);
                samples[i + ch] = self.stage2.processSample(s1, ch);
            }
        }
    }

    pub fn reset(self: *HighPass) void {
        self.stage1.reset();
        self.stage2.reset();
    }
};

pub const NoiseGate = struct {
    threshold_lin: f32,
    close_threshold_lin: f32,
    floor_gain: f32,
    attack_coeff: f32,
    release_coeff: f32,
    hold_samples: u32,
    hold_counter: u32 = 0,
    envelope: f32 = 0.0,
    gain_smooth: f32 = 1.0,
    noise_floor_est: f32 = 0.0005,
    sub_strength: f32 = 0.5,
    enabled: bool = false,

    pub fn init(sample_rate: f32, threshold_db: f32, floor_db: f32) NoiseGate {
        const sr = if (sample_rate > 1000.0) sample_rate else 48000.0;
        const open_db = math.clamp(threshold_db, -75.0, -10.0);
        const close_db = open_db - 5.0;
        return NoiseGate{
            .threshold_lin = dbToLinear(open_db),
            .close_threshold_lin = dbToLinear(close_db),
            .floor_gain = dbToLinear(math.clamp(floor_db, -80.0, -6.0)),
            .attack_coeff = timeToCoeff(2.5, sr),
            .release_coeff = timeToCoeff(110.0, sr),
            .hold_samples = @intFromFloat(sr * 0.045),
            .enabled = false,
        };
    }

    pub fn configureFromStrength(self: *NoiseGate, sample_rate: f32, strength: f32, enabled: bool) void {
        const s = math.clamp(strength, 0.0, 1.0);
        const open_db = -56.0 + s * 26.0;
        const floor_db = -18.0 - s * 36.0;
        const sr = if (sample_rate > 1000.0) sample_rate else 48000.0;

        self.threshold_lin = dbToLinear(open_db);
        self.close_threshold_lin = dbToLinear(open_db - 4.5);
        self.floor_gain = dbToLinear(floor_db);
        self.attack_coeff = timeToCoeff(2.0, sr);
        self.release_coeff = timeToCoeff(90.0 + (1.0 - s) * 60.0, sr);
        self.hold_samples = @intFromFloat(sr * 0.040);
        self.sub_strength = 0.25 + s * 0.65;
        self.enabled = enabled;
    }

    pub fn processBuffer(self: *NoiseGate, samples: []f32, channels: u32) void {
        if (!self.enabled or channels == 0) return;
        const stride: usize = @intCast(channels);
        const env_attack = self.attack_coeff;
        const env_release = self.release_coeff;

        var i: usize = 0;
        while (i + stride <= samples.len) : (i += stride) {
            var peak: f32 = 0.0;
            for (0..stride) |ch| {
                const a = @abs(samples[i + ch]);
                if (a > peak) peak = a;
            }

            if (peak > self.envelope) {
                self.envelope = env_attack * self.envelope + (1.0 - env_attack) * peak;
            } else {
                self.envelope = env_release * self.envelope + (1.0 - env_release) * peak;
            }

            if (self.envelope < self.threshold_lin) {
                self.noise_floor_est = 0.9995 * self.noise_floor_est + 0.0005 * self.envelope;
            } else if (self.envelope < self.noise_floor_est) {
                self.noise_floor_est = 0.99 * self.noise_floor_est + 0.01 * self.envelope;
            }

            var target_gain: f32 = self.floor_gain;
            if (self.envelope >= self.threshold_lin) {
                target_gain = 1.0;
                self.hold_counter = self.hold_samples;
            } else if (self.hold_counter > 0) {
                self.hold_counter -= 1;
                target_gain = 1.0;
            } else if (self.envelope > self.close_threshold_lin) {
                const span = @max(self.threshold_lin - self.close_threshold_lin, 1.0e-6);
                const t = (self.envelope - self.close_threshold_lin) / span;
                const smooth_t = t * t * (3.0 - 2.0 * t);
                target_gain = self.floor_gain + smooth_t * (1.0 - self.floor_gain);
            }

            if (self.envelope > 1.0e-6) {
                const sub = @max(0.0, self.envelope - self.noise_floor_est * (1.0 + self.sub_strength)) / self.envelope;
                target_gain *= math.clamp(sub, self.floor_gain, 1.0);
            }

            const smooth_coeff = if (target_gain > self.gain_smooth) env_attack else env_release;
            self.gain_smooth = smooth_coeff * self.gain_smooth + (1.0 - smooth_coeff) * target_gain;

            for (0..stride) |ch| {
                samples[i + ch] *= self.gain_smooth;
            }
        }
    }

    pub fn reset(self: *NoiseGate) void {
        self.envelope = 0.0;
        self.gain_smooth = 1.0;
        self.hold_counter = 0;
        self.noise_floor_est = 0.0005;
    }
};

pub const Compressor = struct {
    threshold_db: f32,
    ratio: f32,
    knee_width_db: f32 = 6.0,
    makeup_gain_lin: f32 = 1.0,
    attack_coeff: f32,
    release_coeff: f32,
    env_db: f32 = -96.0,
    enabled: bool = false,

    pub fn init(sample_rate: f32, threshold_db: f32, ratio: f32) Compressor {
        const sr = if (sample_rate > 1000.0) sample_rate else 48000.0;
        var comp = Compressor{
            .threshold_db = math.clamp(threshold_db, -45.0, -3.0),
            .ratio = math.clamp(ratio, 1.2, 20.0),
            .attack_coeff = timeToCoeff(6.0, sr),
            .release_coeff = timeToCoeff(110.0, sr),
            .enabled = false,
        };
        comp.updateMakeupGain();
        return comp;
    }

    pub fn updateMakeupGain(self: *Compressor) void {
        const gr_at_zero = (-self.threshold_db) * (1.0 - 1.0 / self.ratio);
        const makeup_db = math.clamp(gr_at_zero * 0.45, 0.0, 12.0);
        self.makeup_gain_lin = dbToLinear(makeup_db);
    }

    pub fn configureFromAmount(self: *Compressor, sample_rate: f32, amount: f32, enabled: bool) void {
        const a = math.clamp(amount, 0.0, 1.0);
        const sr = if (sample_rate > 1000.0) sample_rate else 48000.0;
        self.threshold_db = -12.0 - a * 16.0;
        self.ratio = 1.8 + a * 4.2;
        self.attack_coeff = timeToCoeff(5.0, sr);
        self.release_coeff = timeToCoeff(95.0, sr);
        self.updateMakeupGain();
        self.enabled = enabled;
    }

    pub fn processBuffer(self: *Compressor, samples: []f32, channels: u32) void {
        if (!self.enabled or channels == 0) return;
        const stride: usize = @intCast(channels);
        const half_knee = self.knee_width_db * 0.5;
        const inv_ratio_minus_one = (1.0 / self.ratio) - 1.0;

        var i: usize = 0;
        while (i + stride <= samples.len) : (i += stride) {
            var peak: f32 = 0.0;
            for (0..stride) |ch| {
                const a = @abs(samples[i + ch]);
                if (a > peak) peak = a;
            }

            const input_db = linearToDb(peak);
            const over_db = input_db - self.threshold_db;

            var gain_reduction_db: f32 = 0.0;
            if (over_db <= -half_knee) {
                gain_reduction_db = 0.0;
            } else if (over_db >= half_knee) {
                gain_reduction_db = over_db * inv_ratio_minus_one;
            } else {
                const x = over_db + half_knee;
                gain_reduction_db = inv_ratio_minus_one * (x * x) / (2.0 * self.knee_width_db);
            }

            const coeff = if (gain_reduction_db < self.env_db) self.attack_coeff else self.release_coeff;
            self.env_db = coeff * self.env_db + (1.0 - coeff) * gain_reduction_db;

            const total_gain = dbToLinear(self.env_db) * self.makeup_gain_lin;
            for (0..stride) |ch| {
                samples[i + ch] *= total_gain;
            }
        }
    }

    pub fn reset(self: *Compressor) void {
        self.env_db = 0.0;
    }
};

pub const VoiceChain = struct {
    sample_rate: f32 = 48000.0,
    high_pass: HighPass,
    noise_gate: NoiseGate,
    compressor: Compressor,
    gain_linear: f32 = 1.0,
    volume_multiplier: f32 = 1.0,

    pub fn init(sample_rate: f32) VoiceChain {
        const sr = if (sample_rate > 1000.0) sample_rate else 48000.0;
        var ng = NoiseGate.init(sr, -42.0, -36.0);
        ng.enabled = false;
        var comp = Compressor.init(sr, -20.0, 3.5);
        comp.env_db = 0.0;
        comp.enabled = false;

        var hp = HighPass.init(sr, 85.0);
        hp.enabled = false;

        return VoiceChain{
            .sample_rate = sr,
            .high_pass = hp,
            .noise_gate = ng,
            .compressor = comp,
            .gain_linear = 1.0,
            .volume_multiplier = 1.0,
        };
    }

    pub fn configureMicChain(
        self: *VoiceChain,
        sample_rate: f32,
        noise_suppression: bool,
        ns_strength: f32,
        compressor_enabled: bool,
        comp_amount: f32,
        gain_db: f32,
        volume: f32,
    ) void {
        const sr = if (sample_rate > 1000.0) sample_rate else 48000.0;
        if (@abs(sr - self.sample_rate) > 1.0) {
            self.high_pass = HighPass.init(sr, 85.0);
            self.sample_rate = sr;
        }
        self.high_pass.enabled = noise_suppression;
        self.noise_gate.configureFromStrength(sr, ns_strength, noise_suppression);
        self.compressor.configureFromAmount(sr, comp_amount, compressor_enabled);
        self.gain_linear = dbToLinear(math.clamp(gain_db, -24.0, 30.0));
        self.volume_multiplier = math.clamp(volume, 0.0, 4.0);
    }

    pub inline fn softLimit(x: f32) f32 {
        const ax = @abs(x);
        if (ax <= 0.85) return x;
        const sign: f32 = if (x >= 0.0) 1.0 else -1.0;
        const excess = ax - 0.85;
        return sign * (0.85 + 0.15 * (excess / (0.15 + excess)));
    }

    pub fn processBuffer(self: *VoiceChain, samples: []f32, channels: u32) void {
        if (samples.len == 0 or channels == 0) return;

        self.high_pass.processBuffer(samples, channels);
        self.noise_gate.processBuffer(samples, channels);
        self.compressor.processBuffer(samples, channels);

        const total_scale = self.gain_linear * self.volume_multiplier;
        for (samples) |*s| {
            const v = s.* * total_scale;
            s.* = softLimit(v);
        }
    }
};

test "HighPass attenuates DC and 20Hz while passing 1kHz" {
    const sr: f32 = 48000.0;
    var hp = HighPass.init(sr, 85.0);

    var low_buf: [4800]f32 = undefined;
    for (0..low_buf.len) |i| {
        const t = @as(f32, @floatFromInt(i)) / sr;
        low_buf[i] = @sin(2.0 * math.pi * 20.0 * t);
    }
    hp.processBuffer(&low_buf, 1);

    var low_peak: f32 = 0.0;
    for (low_buf[2400..]) |s| {
        if (@abs(s) > low_peak) low_peak = @abs(s);
    }
    try std.testing.expect(low_peak < 0.15);

    hp.reset();
    var high_buf: [4800]f32 = undefined;
    for (0..high_buf.len) |i| {
        const t = @as(f32, @floatFromInt(i)) / sr;
        high_buf[i] = @sin(2.0 * math.pi * 1000.0 * t);
    }
    hp.processBuffer(&high_buf, 1);

    var high_peak: f32 = 0.0;
    for (high_buf[2400..]) |s| {
        if (@abs(s) > high_peak) high_peak = @abs(s);
    }
    try std.testing.expect(high_peak > 0.95);
}

test "NoiseGate attenuates quiet background hiss and passes speech level" {
    const sr: f32 = 48000.0;
    var ng = NoiseGate.init(sr, -35.0, -40.0);
    ng.configureFromStrength(sr, 0.7, true);

    var quiet_buf: [2400]f32 = undefined;
    for (&quiet_buf) |*s| s.* = 0.002;
    ng.processBuffer(&quiet_buf, 1);
    try std.testing.expect(@abs(quiet_buf[quiet_buf.len - 1]) < 0.0005);

    var loud_buf: [2400]f32 = undefined;
    for (&loud_buf) |*s| s.* = 0.35;
    ng.processBuffer(&loud_buf, 1);
    try std.testing.expect(@abs(loud_buf[loud_buf.len - 1]) > 0.30);
}

test "Compressor reduces dynamic range of loud peaks" {
    const sr: f32 = 48000.0;
    var comp = Compressor.init(sr, -20.0, 4.0);
    comp.configureFromAmount(sr, 0.8, true);

    var loud_buf: [4800]f32 = undefined;
    for (&loud_buf) |*s| s.* = 0.90;
    comp.processBuffer(&loud_buf, 1);
    const compressed_loud = loud_buf[loud_buf.len - 1];
    try std.testing.expect(compressed_loud < 0.90);
    try std.testing.expect(compressed_loud > 0.25);
}

test "VoiceChain applies boost and soft-limits output cleanly" {
    var chain = VoiceChain.init(48000.0);
    chain.configureMicChain(48000.0, true, 0.5, true, 0.5, 18.0, 1.5);

    var buf: [512]f32 = undefined;
    for (0..buf.len) |i| {
        const t = @as(f32, @floatFromInt(i)) / 48000.0;
        buf[i] = 0.5 * @sin(2.0 * math.pi * 440.0 * t);
    }
    chain.processBuffer(&buf, 2);

    for (buf) |s| {
        try std.testing.expect(s >= -1.0 and s <= 1.0);
    }
}
