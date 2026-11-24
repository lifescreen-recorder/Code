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

pub const AiBarkDenoiser = struct {
    const FFT_N: usize = 512;
    const HOP: usize = 256;
    const BINS: usize = FFT_N / 2 + 1;
    const NUM_BANDS: usize = 16;

    const BAND_EDGES = [NUM_BANDS + 1]usize{
        1, 2, 4, 6, 8, 10, 13, 16, 20, 25, 31, 39, 50, 70, 105, 165, 256,
    };

    window: [FFT_N]f32 = undefined,
    in_ring_l: [FFT_N]f32 = @splat(0.0),
    in_ring_r: [FFT_N]f32 = @splat(0.0),
    out_accum_l: [FFT_N]f32 = @splat(0.0),
    out_accum_r: [FFT_N]f32 = @splat(0.0),
    out_ready_l: [HOP]f32 = @splat(0.0),
    out_ready_r: [HOP]f32 = @splat(0.0),
    hop_pos: usize = 0,
    primed: bool = false,

    noise_psd: [NUM_BANDS]f32 = @splat(1.0e-5),
    prev_post_psd: [NUM_BANDS]f32 = @splat(0.0),
    smooth_gain: [NUM_BANDS]f32 = @splat(1.0),
    speech_prob_smooth: f32 = 0.0,
    warmup_frames: u32 = 0,
    strength: f32 = 0.65,
    enabled: bool = false,

    pub fn init() AiBarkDenoiser {
        var d = AiBarkDenoiser{};
        for (0..FFT_N) |n| {
            const phase = (2.0 * math.pi * @as(f32, @floatFromInt(n))) / @as(f32, @floatFromInt(FFT_N));
            d.window[n] = @sqrt(0.5 * (1.0 - @cos(phase)));
        }
        return d;
    }

    pub fn configure(self: *AiBarkDenoiser, strength: f32, enabled: bool) void {
        self.strength = math.clamp(strength, 0.05, 1.0);
        self.enabled = enabled;
    }

    fn fft512(re: *[FFT_N]f32, im: *[FFT_N]f32, inverse: bool) void {
        var j: usize = 0;
        for (1..FFT_N) |i| {
            var bit: usize = FFT_N >> 1;
            while ((j & bit) != 0) {
                j ^= bit;
                bit >>= 1;
            }
            j ^= bit;
            if (i < j) {
                const tr = re[i];
                re[i] = re[j];
                re[j] = tr;
                const ti = im[i];
                im[i] = im[j];
                im[j] = ti;
            }
        }

        var len: usize = 2;
        const sign: f32 = if (inverse) 1.0 else -1.0;
        while (len <= FFT_N) : (len <<= 1) {
            const half = len >> 1;
            const ang = sign * 2.0 * math.pi / @as(f32, @floatFromInt(len));
            const wlen_r = @cos(ang);
            const wlen_i = @sin(ang);

            var i: usize = 0;
            while (i < FFT_N) : (i += len) {
                var wr: f32 = 1.0;
                var wi: f32 = 0.0;
                for (0..half) |k| {
                    const u_r = re[i + k];
                    const u_i = im[i + k];
                    const v_r = re[i + k + half] * wr - im[i + k + half] * wi;
                    const v_i = re[i + k + half] * wi + im[i + k + half] * wr;

                    re[i + k] = u_r + v_r;
                    im[i + k] = u_i + v_i;
                    re[i + k + half] = u_r - v_r;
                    im[i + k + half] = u_i - v_i;

                    const nwr = wr * wlen_r - wi * wlen_i;
                    wi = wr * wlen_i + wi * wlen_r;
                    wr = nwr;
                }
            }
        }

        if (inverse) {
            const inv_n = 1.0 / @as(f32, @floatFromInt(FFT_N));
            for (0..FFT_N) |i| {
                re[i] *= inv_n;
                im[i] *= inv_n;
            }
        }
    }

    fn processFramePair(self: *AiBarkDenoiser, stereo: bool) void {
        var re_l: [FFT_N]f32 = undefined;
        var im_l: [FFT_N]f32 = @splat(0.0);
        var re_r: [FFT_N]f32 = undefined;
        var im_r: [FFT_N]f32 = @splat(0.0);

        for (0..FFT_N) |n| {
            re_l[n] = self.in_ring_l[n] * self.window[n];
            if (stereo) {
                re_r[n] = self.in_ring_r[n] * self.window[n];
            }
        }

        fft512(&re_l, &im_l, false);
        if (stereo) {
            fft512(&re_r, &im_r, false);
        }

        var band_psd: [NUM_BANDS]f32 = @splat(0.0);
        for (0..NUM_BANDS) |b| {
            const k0 = BAND_EDGES[b];
            const k1 = BAND_EDGES[b + 1];
            var sum_p: f32 = 0.0;
            for (k0..k1) |k| {
                var p = re_l[k] * re_l[k] + im_l[k] * im_l[k];
                if (stereo) {
                    p = 0.5 * (p + re_r[k] * re_r[k] + im_r[k] * im_r[k]);
                }
                sum_p += p;
            }
            band_psd[b] = sum_p / @as(f32, @floatFromInt(@max(k1 - k0, 1)));
        }

        if (self.warmup_frames < 8) {
            self.warmup_frames += 1;
            for (0..NUM_BANDS) |b| {
                if (self.warmup_frames == 1) {
                    self.noise_psd[b] = @max(band_psd[b], 1.0e-7);
                } else {
                    self.noise_psd[b] = 0.75 * self.noise_psd[b] + 0.25 * @min(band_psd[b], self.noise_psd[b] * 2.5);
                }
            }
        }

        var voice_band_energy: f32 = 1.0e-7;
        var non_voice_energy: f32 = 1.0e-7;
        var snr_accum: f32 = 0.0;
        for (0..NUM_BANDS) |b| {
            const n_est = @max(self.noise_psd[b], 1.0e-7);
            const post_snr = band_psd[b] / n_est;
            if (b >= 2 and b <= 13) {
                voice_band_energy += band_psd[b];
                snr_accum += @min(post_snr, 40.0);
            } else {
                non_voice_energy += band_psd[b];
            }
        }

        const avg_voice_snr = snr_accum / 12.0;
        const formant_ratio = voice_band_energy / (voice_band_energy + non_voice_energy * 1.5);
        const logit = (avg_voice_snr - (2.2 + self.strength * 1.4)) * 1.35 + (formant_ratio - 0.45) * 3.2;
        const raw_p_speech = 1.0 / (1.0 + @exp(-math.clamp(logit, -12.0, 12.0)));

        const p_coeff: f32 = if (raw_p_speech > self.speech_prob_smooth) 0.35 else 0.82;
        self.speech_prob_smooth = p_coeff * self.speech_prob_smooth + (1.0 - p_coeff) * raw_p_speech;

        const over_sub = 1.0 + self.strength * 2.6;
        const min_floor = dbToLinear(-14.0 - self.strength * 32.0);

        var target_band_gain: [NUM_BANDS]f32 = undefined;
        for (0..NUM_BANDS) |b| {
            const p_cur = band_psd[b];
            if (self.speech_prob_smooth < 0.35) {
                self.noise_psd[b] = 0.96 * self.noise_psd[b] + 0.04 * p_cur;
            } else if (p_cur < self.noise_psd[b]) {
                self.noise_psd[b] = 0.85 * self.noise_psd[b] + 0.15 * p_cur;
            } else {
                self.noise_psd[b] *= 1.0004;
            }

            const n_psd = @max(self.noise_psd[b] * over_sub, 1.0e-8);
            const gamma = p_cur / n_psd;
            const xi_dd = 0.92 * (self.prev_post_psd[b] / n_psd) + 0.08 * @max(gamma - 1.0, 0.0);
            var g_wiener = xi_dd / (1.0 + xi_dd);

            const vad_weight = min_floor + (1.0 - min_floor) * (0.25 + 0.75 * self.speech_prob_smooth);
            g_wiener = @max(g_wiener * vad_weight, min_floor);
            if (b == 0 or b == 15) {
                g_wiener *= (0.55 + 0.45 * self.speech_prob_smooth);
            }
            target_band_gain[b] = math.clamp(g_wiener, min_floor, 1.0);
        }

        for (0..NUM_BANDS) |b| {
            var g = target_band_gain[b];
            if (b > 0 and b + 1 < NUM_BANDS) {
                g = 0.2 * target_band_gain[b - 1] + 0.6 * g + 0.2 * target_band_gain[b + 1];
            }
            const g_coeff: f32 = if (g > self.smooth_gain[b]) 0.30 else 0.72;
            self.smooth_gain[b] = g_coeff * self.smooth_gain[b] + (1.0 - g_coeff) * g;
            self.prev_post_psd[b] = band_psd[b] * self.smooth_gain[b] * self.smooth_gain[b];
        }

        re_l[0] *= min_floor;
        im_l[0] = 0.0;
        re_l[FFT_N / 2] *= self.smooth_gain[NUM_BANDS - 1];
        im_l[FFT_N / 2] = 0.0;
        if (stereo) {
            re_r[0] *= min_floor;
            im_r[0] = 0.0;
            re_r[FFT_N / 2] *= self.smooth_gain[NUM_BANDS - 1];
            im_r[FFT_N / 2] = 0.0;
        }

        for (0..NUM_BANDS) |b| {
            const k0 = BAND_EDGES[b];
            const k1 = BAND_EDGES[b + 1];
            const g0 = self.smooth_gain[b];
            const g1 = if (b + 1 < NUM_BANDS) self.smooth_gain[b + 1] else g0;
            const span_f = @as(f32, @floatFromInt(@max(k1 - k0, 1)));

            for (k0..k1) |k| {
                const t = @as(f32, @floatFromInt(k - k0)) / span_f;
                const gk = g0 + (g1 - g0) * t;
                const mir = FFT_N - k;

                re_l[k] *= gk;
                im_l[k] *= gk;
                re_l[mir] = re_l[k];
                im_l[mir] = -im_l[k];

                if (stereo) {
                    re_r[k] *= gk;
                    im_r[k] *= gk;
                    re_r[mir] = re_r[k];
                    im_r[mir] = -im_r[k];
                }
            }
        }

        fft512(&re_l, &im_l, true);
        if (stereo) {
            fft512(&re_r, &im_r, true);
        }

        for (0..FFT_N) |n| {
            self.out_accum_l[n] += re_l[n] * self.window[n];
            if (stereo) {
                self.out_accum_r[n] += re_r[n] * self.window[n];
            }
        }

        @memcpy(&self.out_ready_l, self.out_accum_l[0..HOP]);
        @memmove(self.out_accum_l[0..HOP], self.out_accum_l[HOP..FFT_N]);
        @memset(self.out_accum_l[HOP..FFT_N], 0.0);

        if (stereo) {
            @memcpy(&self.out_ready_r, self.out_accum_r[0..HOP]);
            @memmove(self.out_accum_r[0..HOP], self.out_accum_r[HOP..FFT_N]);
            @memset(self.out_accum_r[HOP..FFT_N], 0.0);
        }
        self.primed = true;
    }

    pub fn processBuffer(self: *AiBarkDenoiser, samples: []f32, channels: u32) void {
        if (!self.enabled or channels == 0 or samples.len == 0) return;
        const stride: usize = @intCast(channels);
        const stereo = channels >= 2;

        var idx: usize = 0;
        while (idx + stride <= samples.len) : (idx += stride) {
            const in_l = samples[idx];
            const in_r = if (stereo) samples[idx + 1] else in_l;

            self.in_ring_l[HOP + self.hop_pos] = in_l;
            if (stereo) {
                self.in_ring_r[HOP + self.hop_pos] = in_r;
            }

            if (self.primed) {
                samples[idx] = self.out_ready_l[self.hop_pos];
                if (stereo) {
                    samples[idx + 1] = self.out_ready_r[self.hop_pos];
                }
            }

            self.hop_pos += 1;
            if (self.hop_pos >= HOP) {
                self.processFramePair(stereo);
                @memmove(self.in_ring_l[0..HOP], self.in_ring_l[HOP..FFT_N]);
                if (stereo) {
                    @memmove(self.in_ring_r[0..HOP], self.in_ring_r[HOP..FFT_N]);
                }
                self.hop_pos = 0;
            }
        }
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
    ai_denoiser: AiBarkDenoiser,
    noise_gate: NoiseGate,
    compressor: Compressor,
    ns_mode: u32 = 0,
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
            .ai_denoiser = AiBarkDenoiser.init(),
            .noise_gate = ng,
            .compressor = comp,
            .ns_mode = 0,
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
        self.configureMicChainEx(sample_rate, noise_suppression, self.ns_mode, ns_strength, compressor_enabled, comp_amount, gain_db, volume);
    }

    pub fn configureMicChainEx(
        self: *VoiceChain,
        sample_rate: f32,
        noise_suppression: bool,
        ns_mode: u32,
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
        self.ns_mode = ns_mode;
        self.high_pass.enabled = noise_suppression;
        if (ns_mode == 0) {
            self.ai_denoiser.configure(ns_strength, noise_suppression);
            self.noise_gate.configureFromStrength(sr, ns_strength * 0.45, noise_suppression);
        } else {
            self.ai_denoiser.configure(ns_strength, false);
            self.noise_gate.configureFromStrength(sr, ns_strength, noise_suppression);
        }
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
        self.ai_denoiser.processBuffer(samples, channels);
        self.noise_gate.processBuffer(samples, channels);
        self.compressor.processBuffer(samples, channels);

        const total_scale = self.gain_linear * self.volume_multiplier;
        for (samples) |*s| {
            const v = s.* * total_scale;
            s.* = softLimit(v);
        }
    }
};

pub const SystemChain = struct {
    sample_rate: f32 = 48000.0,
    high_pass: HighPass,
    compressor: Compressor,
    gain_linear: f32 = 1.0,
    volume_multiplier: f32 = 1.0,

    pub fn init(sample_rate: f32) SystemChain {
        const sr = if (sample_rate > 1000.0) sample_rate else 48000.0;
        var hp = HighPass.init(sr, 35.0);
        hp.enabled = false;
        var comp = Compressor.init(sr, -18.0, 3.0);
        comp.env_db = 0.0;
        comp.enabled = false;

        return SystemChain{
            .sample_rate = sr,
            .high_pass = hp,
            .compressor = comp,
            .gain_linear = 1.0,
            .volume_multiplier = 1.0,
        };
    }

    pub fn configureSystemChain(
        self: *SystemChain,
        sample_rate: f32,
        clarity_hp: bool,
        compressor_enabled: bool,
        comp_amount: f32,
        gain_db: f32,
        volume: f32,
    ) void {
        const sr = if (sample_rate > 1000.0) sample_rate else 48000.0;
        if (@abs(sr - self.sample_rate) > 1.0) {
            self.high_pass = HighPass.init(sr, 35.0);
            self.sample_rate = sr;
        }
        self.high_pass.enabled = clarity_hp;
        self.compressor.configureFromAmount(sr, comp_amount, compressor_enabled);
        self.gain_linear = dbToLinear(math.clamp(gain_db, -12.0, 18.0));
        self.volume_multiplier = math.clamp(volume, 0.0, 4.0);
    }

    pub fn processBuffer(self: *SystemChain, samples: []f32, channels: u32) void {
        if (samples.len == 0 or channels == 0) return;

        self.high_pass.processBuffer(samples, channels);
        self.compressor.processBuffer(samples, channels);

        const total_scale = self.gain_linear * self.volume_multiplier;
        for (samples) |*s| {
            const v = s.* * total_scale;
            s.* = VoiceChain.softLimit(v);
        }
    }
};

var g_test_voice_chain: VoiceChain = VoiceChain.init(48000.0);

pub export fn lifescreen_dsp_test_mic_process(
    samples: [*]f32,
    frame_count: u32,
    channels: u32,
    sample_rate: f32,
    noise_suppression: bool,
    ns_strength: f32,
    compressor: bool,
    comp_amount: f32,
    gain_db: f32,
    volume: f32,
) callconv(.c) void {
    const total = @as(usize, frame_count) * @as(usize, channels);
    if (total == 0) return;
    g_test_voice_chain.configureMicChain(
        sample_rate,
        noise_suppression,
        ns_strength,
        compressor,
        comp_amount,
        gain_db,
        volume,
    );
    g_test_voice_chain.processBuffer(samples[0..total], channels);
}

pub export fn lifescreen_dsp_test_mic_process_ex(
    samples: [*]f32,
    frame_count: u32,
    channels: u32,
    sample_rate: f32,
    noise_suppression: bool,
    ns_mode: u32,
    ns_strength: f32,
    compressor: bool,
    comp_amount: f32,
    gain_db: f32,
    volume: f32,
) callconv(.c) void {
    const total = @as(usize, frame_count) * @as(usize, channels);
    if (total == 0) return;
    g_test_voice_chain.configureMicChainEx(
        sample_rate,
        noise_suppression,
        ns_mode,
        ns_strength,
        compressor,
        comp_amount,
        gain_db,
        volume,
    );
    g_test_voice_chain.processBuffer(samples[0..total], channels);
}

test "AiBarkDenoiser suppresses steady noise floor and preserves speech tone" {
    var ai = AiBarkDenoiser.init();
    ai.configure(0.8, true);

    var noise_buf: [4096]f32 = undefined;
    var seed: u32 = 12345;
    for (&noise_buf) |*s| {
        seed = seed *% 1664525 +% 1013904223;
        const r = @as(f32, @floatFromInt(seed & 0xFFFF)) / 32768.0 - 1.0;
        s.* = r * 0.02;
    }
    ai.processBuffer(&noise_buf, 1);

    var tail_rms: f32 = 0.0;
    for (noise_buf[3072..]) |s| {
        tail_rms += s * s;
    }
    tail_rms = @sqrt(tail_rms / 1024.0);
    try std.testing.expect(tail_rms < 0.005);

    var speech_buf: [4096]f32 = undefined;
    for (0..speech_buf.len) |i| {
        const t = @as(f32, @floatFromInt(i)) / 48000.0;
        speech_buf[i] = 0.4 * @sin(2.0 * math.pi * 600.0 * t);
    }
    ai.processBuffer(&speech_buf, 1);

    var speech_peak: f32 = 0.0;
    for (speech_buf[2048..]) |s| {
        if (@abs(s) > speech_peak) speech_peak = @abs(s);
    }
    try std.testing.expect(speech_peak > 0.25);
}

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
