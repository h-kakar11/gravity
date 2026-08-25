// Feeds canned "-progress pipe:1"-style text directly into FFmpegProgressParser -- no
// real ffmpeg process involved (spec section 36).

#include "engines/ffmpeg/FFmpegProgressParser.h"

#include <gtest/gtest.h>

using mediatool::media::FFmpegProgressParser;

TEST(FFmpegProgressParserTest, NoProgressUntilBlockTerminates) {
    FFmpegProgressParser parser(100.0);
    parser.FeedLine("frame=10");
    parser.FeedLine("out_time_us=5000000");
    EXPECT_FALSE(parser.TakeProgressIfReady().has_value());
}

TEST(FFmpegProgressParserTest, ParsesContinueBlockWithDuration) {
    FFmpegProgressParser parser(100.0);
    parser.FeedLine("frame=120");
    parser.FeedLine("fps=30.00");
    parser.FeedLine("bitrate=1200.0kbits/s");
    parser.FeedLine("total_size=1500000");
    parser.FeedLine("out_time_us=45000000");  // 45 seconds
    parser.FeedLine("speed=1.50x");
    parser.FeedLine("progress=continue");

    auto progress = parser.TakeProgressIfReady();
    ASSERT_TRUE(progress.has_value());
    ASSERT_TRUE(progress->percentage.has_value());
    EXPECT_NEAR(*progress->percentage, 45.0, 0.001);
    ASSERT_TRUE(progress->processedBytes.has_value());
    EXPECT_EQ(*progress->processedBytes, 1500000u);
    ASSERT_TRUE(progress->etaSeconds.has_value());
    EXPECT_NEAR(*progress->etaSeconds, (100.0 - 45.0) / 1.5, 0.001);
    ASSERT_TRUE(progress->currentItem.has_value());
    EXPECT_EQ(*progress->currentItem, "frame 120");
    EXPECT_NE(progress->statusMessage.find("120"), std::string::npos);

    // Consumed -- a second call before feeding another block returns nothing new.
    EXPECT_FALSE(parser.TakeProgressIfReady().has_value());
}

TEST(FFmpegProgressParserTest, EstimatesSpeedBytesPerSecondFromInputBitrateWhenProvided) {
    // 128 kbit/s input, encoding at 2x realtime -> 128000/8 * 2 = 32000 bytes/sec.
    FFmpegProgressParser parser(100.0, /*inputBitrateBps=*/128000.0);
    parser.FeedLine("out_time_us=10000000");
    parser.FeedLine("speed=2.0x");
    parser.FeedLine("progress=continue");

    auto progress = parser.TakeProgressIfReady();
    ASSERT_TRUE(progress.has_value());
    ASSERT_TRUE(progress->speedBytesPerSecond.has_value());
    EXPECT_NEAR(*progress->speedBytesPerSecond, 32000.0, 0.001);
}

TEST(FFmpegProgressParserTest, SpeedBytesPerSecondUnsetWithoutInputBitrate) {
    FFmpegProgressParser parser(100.0);  // no inputBitrateBps supplied
    parser.FeedLine("out_time_us=10000000");
    parser.FeedLine("speed=2.0x");
    parser.FeedLine("progress=continue");

    auto progress = parser.TakeProgressIfReady();
    ASSERT_TRUE(progress.has_value());
    EXPECT_FALSE(progress->speedBytesPerSecond.has_value());
}

TEST(FFmpegProgressParserTest, OutTimeMsIsInterpretedAsMicroseconds) {
    // ffmpeg's out_time_ms field is a long-standing misnomer -- its value is actually
    // microseconds. 4500000 -> 4.5 seconds, not 4500 seconds.
    FFmpegProgressParser parser(9.0);
    parser.FeedLine("out_time_ms=4500000");
    parser.FeedLine("progress=continue");

    auto progress = parser.TakeProgressIfReady();
    ASSERT_TRUE(progress.has_value());
    ASSERT_TRUE(progress->percentage.has_value());
    EXPECT_NEAR(*progress->percentage, 50.0, 0.001);  // 4.5 / 9.0 * 100
}

TEST(FFmpegProgressParserTest, PercentageUnsetWithoutKnownDuration) {
    FFmpegProgressParser parser(std::nullopt);
    parser.FeedLine("out_time_us=4500000");
    parser.FeedLine("progress=continue");

    auto progress = parser.TakeProgressIfReady();
    ASSERT_TRUE(progress.has_value());
    EXPECT_FALSE(progress->percentage.has_value());
    EXPECT_FALSE(progress->etaSeconds.has_value());
}

TEST(FFmpegProgressParserTest, EndBlockForcesFullPercentage) {
    FFmpegProgressParser parser(100.0);
    parser.FeedLine("out_time_us=10000000");  // only 10% through by time, but this is the last block
    parser.FeedLine("progress=end");

    auto progress = parser.TakeProgressIfReady();
    ASSERT_TRUE(progress.has_value());
    ASSERT_TRUE(progress->percentage.has_value());
    EXPECT_NEAR(*progress->percentage, 100.0, 0.001);
    EXPECT_EQ(progress->statusMessage, "Completed");
}

TEST(FFmpegProgressParserTest, MalformedLineIsIgnoredNotFatal) {
    FFmpegProgressParser parser(100.0);
    parser.FeedLine("this line has no equals sign");
    parser.FeedLine("");
    parser.FeedLine("progress=continue");

    auto progress = parser.TakeProgressIfReady();
    ASSERT_TRUE(progress.has_value());
    EXPECT_FALSE(progress->currentItem.has_value());
    EXPECT_FALSE(progress->percentage.has_value());
}

TEST(FFmpegProgressParserTest, MultipleBlocksEachProduceOneProgress) {
    FFmpegProgressParser parser(100.0);

    parser.FeedLine("out_time_us=10000000");
    parser.FeedLine("progress=continue");
    auto first = parser.TakeProgressIfReady();
    ASSERT_TRUE(first.has_value());
    EXPECT_NEAR(*first->percentage, 10.0, 0.001);

    parser.FeedLine("out_time_us=20000000");
    parser.FeedLine("progress=continue");
    auto second = parser.TakeProgressIfReady();
    ASSERT_TRUE(second.has_value());
    EXPECT_NEAR(*second->percentage, 20.0, 0.001);
}
