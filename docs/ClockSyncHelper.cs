using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace MSLCOverlay.Core
{
    /// <summary>
    /// ClockSyncHelper - Lớp hỗ trợ hiệu chỉnh sai lệch clock (0-1 words drift) 
    /// giữa Hardware Audio Playback Playhead và Azure Speech SDK Ticks.
    /// </summary>
    public class ClockSyncHelper
    {
        // Import API Windows để lấy system time chính xác cao (100ns units)
        [DllImport("kernel32.dll")]
        private static extern void GetSystemTimePreciseAsFileTime(out long lpSystemTimeAsFileTime);

        // Mốc thời gian hệ thống chính xác cao khi session hoặc speech bắt đầu (100ns units)
        private long _sessionStartPreciseTicks = 0;
        
        // Độ trễ trung bình của đường truyền Named Pipe IPC (được đo đạc thực nghiệm là 0.11ms ~ 1100 ticks)
        private const long AverageIpcLatencyTicks = 1100;

        /// <summary>
        /// Đăng ký mốc neo thời gian (Anchor) khi nhận được sự kiện SessionStarted hoặc SpeechStartDetected.
        /// </summary>
        public void AnchorSessionStart()
        {
            GetSystemTimePreciseAsFileTime(out _sessionStartPreciseTicks);
        }

        /// <summary>
        /// Tính toán vị trí playhead thực tế (Target Playback Position) cần nhảy tới trong file âm thanh.
        /// </summary>
        /// <param name="wordOffsetSdkTicks">Mốc offset của từ/phân đoạn lấy từ Azure Speech SDK (100ns ticks)</param>
        /// <param name="packetReceivedPreciseTicks">Thời điểm hệ thống chính xác nhận được gói tin (hoặc 0 nếu dùng thời điểm hiện tại)</param>
        /// <returns>Mốc thời gian playback target tính bằng mili-giây (ms)</returns>
        public double CalculateTargetPlaybackMs(long wordOffsetSdkTicks, long packetReceivedPreciseTicks = 0)
        {
            if (_sessionStartPreciseTicks == 0)
            {
                // Nếu chưa có anchor, fallback về offset thuần của SDK
                return wordOffsetSdkTicks / 10000.0;
            }

            long currentPrecise;
            if (packetReceivedPreciseTicks > 0)
            {
                currentPrecise = packetReceivedPreciseTicks;
            }
            else
            {
                GetSystemTimePreciseAsFileTime(out currentPrecise);
            }

            // Khoảng thời gian thực tế đã trôi qua kể từ khi nói đến khi xử lý gói tin hiện tại
            long realElapsedTicks = currentPrecise - _sessionStartPreciseTicks;

            // Hiệu chỉnh độ trễ Named Pipe truyền dẫn
            long correctedElapsedTicks = realElapsedTicks - AverageIpcLatencyTicks;

            // Tính toán độ lệch giữa SDK clock (Audio Offset) và System wall-clock
            long driftTicks = correctedElapsedTicks - wordOffsetSdkTicks;

            // Tinh chỉnh Target Playhead: lấy offset đích trừ đi độ lệch trôi (drift)
            long targetPlayheadTicks = wordOffsetSdkTicks - driftTicks;

            if (targetPlayheadTicks < 0)
            {
                targetPlayheadTicks = 0;
            }

            // Chuyển đổi từ 100ns ticks sang ms (1ms = 10,000 ticks)
            return targetPlayheadTicks / 10000.0;
        }

        /// <summary>
        /// Kiểm tra xem một word có khớp với playhead hiện tại hay không (trong khoảng buffer cho phép).
        /// </summary>
        public bool IsWordInPlaybackWindow(long wordOffsetSdkTicks, double currentPlaybackMs, double bufferWindowMs = 150.0)
        {
            double targetMs = CalculateTargetPlaybackMs(wordOffsetSdkTicks);
            double diff = Math.Abs(currentPlaybackMs - targetMs);
            return diff <= bufferWindowMs;
        }
    }
}
