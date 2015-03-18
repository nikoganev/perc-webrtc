/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/audio_coding/neteq/audio_decoder_impl.h"

#include <assert.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "webrtc/base/scoped_ptr.h"
#include "webrtc/modules/audio_coding/codecs/g711/include/audio_encoder_pcm.h"
#include "webrtc/modules/audio_coding/codecs/g722/include/audio_encoder_g722.h"
#include "webrtc/modules/audio_coding/codecs/ilbc/interface/audio_encoder_ilbc.h"
#include "webrtc/modules/audio_coding/codecs/isac/fix/interface/audio_encoder_isacfix.h"
#include "webrtc/modules/audio_coding/codecs/isac/main/interface/audio_encoder_isac.h"
#include "webrtc/modules/audio_coding/codecs/opus/interface/audio_encoder_opus.h"
#include "webrtc/modules/audio_coding/codecs/pcm16b/include/audio_encoder_pcm16b.h"
#include "webrtc/modules/audio_coding/neteq/tools/resample_input_audio_file.h"
#include "webrtc/system_wrappers/interface/data_log.h"
#include "webrtc/test/testsupport/fileutils.h"

namespace webrtc {

namespace {
// The absolute difference between the input and output (the first channel) is
// compared vs |tolerance|. The parameter |delay| is used to correct for codec
// delays.
void CompareInputOutput(const std::vector<int16_t>& input,
                        const std::vector<int16_t>& output,
                        size_t num_samples,
                        size_t channels,
                        int tolerance,
                        int delay) {
  ASSERT_LE(num_samples, input.size());
  ASSERT_LE(num_samples * channels, output.size());
  for (unsigned int n = 0; n < num_samples - delay; ++n) {
    ASSERT_NEAR(input[n], output[channels * n + delay], tolerance)
        << "Exit test on first diff; n = " << n;
    DataLog::InsertCell("CodecTest", "input", input[n]);
    DataLog::InsertCell("CodecTest", "output", output[channels * n]);
    DataLog::NextRow("CodecTest");
  }
}

// The absolute difference between the first two channels in |output| is
// compared vs |tolerance|.
void CompareTwoChannels(const std::vector<int16_t>& output,
                        size_t samples_per_channel,
                        size_t channels,
                        int tolerance) {
  ASSERT_GE(channels, 2u);
  ASSERT_LE(samples_per_channel * channels, output.size());
  for (unsigned int n = 0; n < samples_per_channel; ++n)
    ASSERT_NEAR(output[channels * n], output[channels * n + 1], tolerance)
        << "Stereo samples differ.";
}

// Calculates mean-squared error between input and output (the first channel).
// The parameter |delay| is used to correct for codec delays.
double MseInputOutput(const std::vector<int16_t>& input,
                      const std::vector<int16_t>& output,
                      size_t num_samples,
                      size_t channels,
                      int delay) {
  assert(delay < static_cast<int>(num_samples));
  assert(num_samples <= input.size());
  assert(num_samples * channels <= output.size());
  if (num_samples == 0)
    return 0.0;
  double squared_sum = 0.0;
  for (unsigned int n = 0; n < num_samples - delay; ++n) {
    squared_sum += (input[n] - output[channels * n + delay]) *
                   (input[n] - output[channels * n + delay]);
  }
  return squared_sum / (num_samples - delay);
}
}  // namespace

class AudioDecoderTest : public ::testing::Test {
 protected:
  AudioDecoderTest()
      : input_audio_(webrtc::test::ProjectRootPath() +
                         "resources/audio_coding/testfile32kHz.pcm",
                     32000),
        codec_input_rate_hz_(32000),  // Legacy default value.
        encoded_(NULL),
        frame_size_(0),
        data_length_(0),
        encoded_bytes_(0),
        channels_(1),
        payload_type_(17),
        decoder_(NULL) {}

  virtual ~AudioDecoderTest() {}

  virtual void SetUp() {
    if (audio_encoder_)
      codec_input_rate_hz_ = audio_encoder_->SampleRateHz();
    // Create arrays.
    ASSERT_GT(data_length_, 0u) << "The test must set data_length_ > 0";
    // Longest encoded data is produced by PCM16b with 2 bytes per sample.
    encoded_ = new uint8_t[data_length_ * 2];
    // Logging to view input and output in Matlab.
    // Use 'gyp -Denable_data_logging=1' to enable logging.
    DataLog::CreateLog();
    DataLog::AddTable("CodecTest");
    DataLog::AddColumn("CodecTest", "input", 1);
    DataLog::AddColumn("CodecTest", "output", 1);
  }

  virtual void TearDown() {
    delete decoder_;
    decoder_ = NULL;
    // Delete arrays.
    delete [] encoded_;
    encoded_ = NULL;
    // Close log.
    DataLog::ReturnLog();
  }

  virtual void InitEncoder() { }

  // TODO(henrik.lundin) Change return type to size_t once most/all overriding
  // implementations are gone.
  virtual int EncodeFrame(const int16_t* input,
                          size_t input_len_samples,
                          uint8_t* output) {
    encoded_info_.encoded_bytes = 0;
    const size_t samples_per_10ms = audio_encoder_->SampleRateHz() / 100;
    CHECK_EQ(samples_per_10ms * audio_encoder_->Num10MsFramesInNextPacket(),
             input_len_samples);
    rtc::scoped_ptr<int16_t[]> interleaved_input(
        new int16_t[channels_ * samples_per_10ms]);
    for (int i = 0; i < audio_encoder_->Num10MsFramesInNextPacket(); ++i) {
      EXPECT_EQ(0u, encoded_info_.encoded_bytes);

      // Duplicate the mono input signal to however many channels the test
      // wants.
      test::InputAudioFile::DuplicateInterleaved(input + i * samples_per_10ms,
                                                 samples_per_10ms, channels_,
                                                 interleaved_input.get());

      audio_encoder_->Encode(0, interleaved_input.get(),
                             audio_encoder_->SampleRateHz() / 100,
                             data_length_ * 2, output, &encoded_info_);
    }
    EXPECT_EQ(payload_type_, encoded_info_.payload_type);
    return static_cast<int>(encoded_info_.encoded_bytes);
  }

  // Encodes and decodes audio. The absolute difference between the input and
  // output is compared vs |tolerance|, and the mean-squared error is compared
  // with |mse|. The encoded stream should contain |expected_bytes|. For stereo
  // audio, the absolute difference between the two channels is compared vs
  // |channel_diff_tolerance|.
  void EncodeDecodeTest(size_t expected_bytes, int tolerance, double mse,
                        int delay = 0, int channel_diff_tolerance = 0) {
    ASSERT_GE(tolerance, 0) << "Test must define a tolerance >= 0";
    ASSERT_GE(channel_diff_tolerance, 0) <<
        "Test must define a channel_diff_tolerance >= 0";
    size_t processed_samples = 0u;
    encoded_bytes_ = 0u;
    InitEncoder();
    EXPECT_EQ(0, decoder_->Init());
    std::vector<int16_t> input;
    std::vector<int16_t> decoded;
    while (processed_samples + frame_size_ <= data_length_) {
      // Extend input vector with |frame_size_|.
      input.resize(input.size() + frame_size_, 0);
      // Read from input file.
      ASSERT_GE(input.size() - processed_samples, frame_size_);
      ASSERT_TRUE(input_audio_.Read(
          frame_size_, codec_input_rate_hz_, &input[processed_samples]));
      size_t enc_len = EncodeFrame(
          &input[processed_samples], frame_size_, &encoded_[encoded_bytes_]);
      // Make sure that frame_size_ * channels_ samples are allocated and free.
      decoded.resize((processed_samples + frame_size_) * channels_, 0);
      AudioDecoder::SpeechType speech_type;
      size_t dec_len = decoder_->Decode(
          &encoded_[encoded_bytes_], enc_len, codec_input_rate_hz_,
          frame_size_ * channels_ * sizeof(int16_t),
          &decoded[processed_samples * channels_], &speech_type);
      EXPECT_EQ(frame_size_ * channels_, dec_len);
      encoded_bytes_ += enc_len;
      processed_samples += frame_size_;
    }
    // For some codecs it doesn't make sense to check expected number of bytes,
    // since the number can vary for different platforms. Opus and iSAC are
    // such codecs. In this case expected_bytes is set to 0.
    if (expected_bytes) {
      EXPECT_EQ(expected_bytes, encoded_bytes_);
    }
    CompareInputOutput(
        input, decoded, processed_samples, channels_, tolerance, delay);
    if (channels_ == 2)
      CompareTwoChannels(
          decoded, processed_samples, channels_, channel_diff_tolerance);
    EXPECT_LE(
        MseInputOutput(input, decoded, processed_samples, channels_, delay),
        mse);
  }

  // Encodes a payload and decodes it twice with decoder re-init before each
  // decode. Verifies that the decoded result is the same.
  void ReInitTest() {
    InitEncoder();
    rtc::scoped_ptr<int16_t[]> input(new int16_t[frame_size_]);
    ASSERT_TRUE(
        input_audio_.Read(frame_size_, codec_input_rate_hz_, input.get()));
    size_t enc_len = EncodeFrame(input.get(), frame_size_, encoded_);
    size_t dec_len;
    AudioDecoder::SpeechType speech_type1, speech_type2;
    EXPECT_EQ(0, decoder_->Init());
    rtc::scoped_ptr<int16_t[]> output1(new int16_t[frame_size_ * channels_]);
    dec_len = decoder_->Decode(encoded_, enc_len, codec_input_rate_hz_,
                               frame_size_ * channels_ * sizeof(int16_t),
                               output1.get(), &speech_type1);
    ASSERT_LE(dec_len, frame_size_ * channels_);
    EXPECT_EQ(frame_size_ * channels_, dec_len);
    // Re-init decoder and decode again.
    EXPECT_EQ(0, decoder_->Init());
    rtc::scoped_ptr<int16_t[]> output2(new int16_t[frame_size_ * channels_]);
    dec_len = decoder_->Decode(encoded_, enc_len, codec_input_rate_hz_,
                               frame_size_ * channels_ * sizeof(int16_t),
                               output2.get(), &speech_type2);
    ASSERT_LE(dec_len, frame_size_ * channels_);
    EXPECT_EQ(frame_size_ * channels_, dec_len);
    for (unsigned int n = 0; n < frame_size_; ++n) {
      ASSERT_EQ(output1[n], output2[n]) << "Exit test on first diff; n = " << n;
    }
    EXPECT_EQ(speech_type1, speech_type2);
  }

  // Call DecodePlc and verify that the correct number of samples is produced.
  void DecodePlcTest() {
    InitEncoder();
    rtc::scoped_ptr<int16_t[]> input(new int16_t[frame_size_]);
    ASSERT_TRUE(
        input_audio_.Read(frame_size_, codec_input_rate_hz_, input.get()));
    size_t enc_len = EncodeFrame(input.get(), frame_size_, encoded_);
    AudioDecoder::SpeechType speech_type;
    EXPECT_EQ(0, decoder_->Init());
    rtc::scoped_ptr<int16_t[]> output(new int16_t[frame_size_ * channels_]);
    size_t dec_len = decoder_->Decode(encoded_, enc_len, codec_input_rate_hz_,
                                      frame_size_ * channels_ * sizeof(int16_t),
                                      output.get(), &speech_type);
    EXPECT_EQ(frame_size_ * channels_, dec_len);
    // Call DecodePlc and verify that we get one frame of data.
    // (Overwrite the output from the above Decode call, but that does not
    // matter.)
    dec_len = decoder_->DecodePlc(1, output.get());
    EXPECT_EQ(frame_size_ * channels_, dec_len);
  }

  test::ResampleInputAudioFile input_audio_;
  int codec_input_rate_hz_;
  uint8_t* encoded_;
  size_t frame_size_;
  size_t data_length_;
  size_t encoded_bytes_;
  size_t channels_;
  const int payload_type_;
  AudioEncoder::EncodedInfo encoded_info_;
  AudioDecoder* decoder_;
  rtc::scoped_ptr<AudioEncoder> audio_encoder_;
};

class AudioDecoderPcmUTest : public AudioDecoderTest {
 protected:
  AudioDecoderPcmUTest() : AudioDecoderTest() {
    frame_size_ = 160;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderPcmU;
    AudioEncoderPcmU::Config config;
    config.frame_size_ms = static_cast<int>(frame_size_ / 8);
    config.payload_type = payload_type_;
    audio_encoder_.reset(new AudioEncoderPcmU(config));
  }
};

class AudioDecoderPcmATest : public AudioDecoderTest {
 protected:
  AudioDecoderPcmATest() : AudioDecoderTest() {
    frame_size_ = 160;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderPcmA;
    AudioEncoderPcmA::Config config;
    config.frame_size_ms = static_cast<int>(frame_size_ / 8);
    config.payload_type = payload_type_;
    audio_encoder_.reset(new AudioEncoderPcmA(config));
  }
};

class AudioDecoderPcm16BTest : public AudioDecoderTest {
 protected:
  AudioDecoderPcm16BTest() : AudioDecoderTest() {
    codec_input_rate_hz_ = 16000;
    frame_size_ = 20 * codec_input_rate_hz_ / 1000;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderPcm16B;
    assert(decoder_);
    AudioEncoderPcm16B::Config config;
    config.sample_rate_hz = codec_input_rate_hz_;
    config.frame_size_ms =
        static_cast<int>(frame_size_ / (config.sample_rate_hz / 1000));
    config.payload_type = payload_type_;
    audio_encoder_.reset(new AudioEncoderPcm16B(config));
  }
};

class AudioDecoderIlbcTest : public AudioDecoderTest {
 protected:
  AudioDecoderIlbcTest() : AudioDecoderTest() {
    codec_input_rate_hz_ = 8000;
    frame_size_ = 240;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderIlbc;
    assert(decoder_);
    AudioEncoderIlbc::Config config;
    config.frame_size_ms = 30;
    config.payload_type = payload_type_;
    audio_encoder_.reset(new AudioEncoderIlbc(config));
  }

  // Overload the default test since iLBC's function WebRtcIlbcfix_NetEqPlc does
  // not return any data. It simply resets a few states and returns 0.
  void DecodePlcTest() {
    InitEncoder();
    rtc::scoped_ptr<int16_t[]> input(new int16_t[frame_size_]);
    ASSERT_TRUE(
        input_audio_.Read(frame_size_, codec_input_rate_hz_, input.get()));
    size_t enc_len = EncodeFrame(input.get(), frame_size_, encoded_);
    AudioDecoder::SpeechType speech_type;
    EXPECT_EQ(0, decoder_->Init());
    rtc::scoped_ptr<int16_t[]> output(new int16_t[frame_size_ * channels_]);
    size_t dec_len = decoder_->Decode(encoded_, enc_len, codec_input_rate_hz_,
                                      frame_size_ * channels_ * sizeof(int16_t),
                                      output.get(), &speech_type);
    EXPECT_EQ(frame_size_, dec_len);
    // Simply call DecodePlc and verify that we get 0 as return value.
    EXPECT_EQ(0, decoder_->DecodePlc(1, output.get()));
  }
};

class AudioDecoderIsacFloatTest : public AudioDecoderTest {
 protected:
  AudioDecoderIsacFloatTest() : AudioDecoderTest() {
    codec_input_rate_hz_ = 16000;
    frame_size_ = 480;
    data_length_ = 10 * frame_size_;
    AudioEncoderDecoderIsac::Config config;
    config.payload_type = payload_type_;
    config.sample_rate_hz = codec_input_rate_hz_;
    config.frame_size_ms =
        1000 * static_cast<int>(frame_size_) / codec_input_rate_hz_;

    // We need to create separate AudioEncoderDecoderIsac objects for encoding
    // and decoding, because the test class destructor destroys them both.
    audio_encoder_.reset(new AudioEncoderDecoderIsac(config));
    decoder_ = new AudioEncoderDecoderIsac(config);
  }
};

class AudioDecoderIsacSwbTest : public AudioDecoderTest {
 protected:
  AudioDecoderIsacSwbTest() : AudioDecoderTest() {
    codec_input_rate_hz_ = 32000;
    frame_size_ = 960;
    data_length_ = 10 * frame_size_;
    AudioEncoderDecoderIsac::Config config;
    config.payload_type = payload_type_;
    config.sample_rate_hz = codec_input_rate_hz_;
    config.frame_size_ms =
        1000 * static_cast<int>(frame_size_) / codec_input_rate_hz_;

    // We need to create separate AudioEncoderDecoderIsac objects for encoding
    // and decoding, because the test class destructor destroys them both.
    audio_encoder_.reset(new AudioEncoderDecoderIsac(config));
    decoder_ = new AudioEncoderDecoderIsac(config);
  }
};

class AudioDecoderIsacFixTest : public AudioDecoderTest {
 protected:
  AudioDecoderIsacFixTest() : AudioDecoderTest() {
    codec_input_rate_hz_ = 16000;
    frame_size_ = 480;
    data_length_ = 10 * frame_size_;
    AudioEncoderDecoderIsacFix::Config config;
    config.payload_type = payload_type_;
    config.sample_rate_hz = codec_input_rate_hz_;
    config.frame_size_ms =
        1000 * static_cast<int>(frame_size_) / codec_input_rate_hz_;

    // We need to create separate AudioEncoderDecoderIsacFix objects for
    // encoding and decoding, because the test class destructor destroys them
    // both.
    audio_encoder_.reset(new AudioEncoderDecoderIsacFix(config));
    decoder_ = new AudioEncoderDecoderIsacFix(config);
  }
};

class AudioDecoderG722Test : public AudioDecoderTest {
 protected:
  AudioDecoderG722Test() : AudioDecoderTest() {
    codec_input_rate_hz_ = 16000;
    frame_size_ = 160;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderG722;
    assert(decoder_);
    AudioEncoderG722::Config config;
    config.frame_size_ms = 10;
    config.payload_type = payload_type_;
    config.num_channels = 1;
    audio_encoder_.reset(new AudioEncoderG722(config));
  }
};

class AudioDecoderG722StereoTest : public AudioDecoderTest {
 protected:
  AudioDecoderG722StereoTest() : AudioDecoderTest() {
    channels_ = 2;
    codec_input_rate_hz_ = 16000;
    frame_size_ = 160;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderG722Stereo;
    assert(decoder_);
    AudioEncoderG722::Config config;
    config.frame_size_ms = 10;
    config.payload_type = payload_type_;
    config.num_channels = 2;
    audio_encoder_.reset(new AudioEncoderG722(config));
  }
};

class AudioDecoderOpusTest : public AudioDecoderTest {
 protected:
  AudioDecoderOpusTest() : AudioDecoderTest() {
    codec_input_rate_hz_ = 48000;
    frame_size_ = 480;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderOpus(1);
    AudioEncoderOpus::Config config;
    config.frame_size_ms = static_cast<int>(frame_size_) / 48;
    config.payload_type = payload_type_;
    config.application = AudioEncoderOpus::kVoip;
    audio_encoder_.reset(new AudioEncoderOpus(config));
  }
};

class AudioDecoderOpusStereoTest : public AudioDecoderOpusTest {
 protected:
  AudioDecoderOpusStereoTest() : AudioDecoderOpusTest() {
    channels_ = 2;
    delete decoder_;
    decoder_ = new AudioDecoderOpus(2);
    AudioEncoderOpus::Config config;
    config.frame_size_ms = static_cast<int>(frame_size_) / 48;
    config.num_channels = 2;
    config.payload_type = payload_type_;
    config.application = AudioEncoderOpus::kAudio;
    audio_encoder_.reset(new AudioEncoderOpus(config));
  }
};

TEST_F(AudioDecoderPcmUTest, EncodeDecode) {
  int tolerance = 251;
  double mse = 1734.0;
  EXPECT_TRUE(CodecSupported(kDecoderPCMu));
  EncodeDecodeTest(data_length_, tolerance, mse);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderPcmATest, EncodeDecode) {
  int tolerance = 308;
  double mse = 1931.0;
  EXPECT_TRUE(CodecSupported(kDecoderPCMa));
  EncodeDecodeTest(data_length_, tolerance, mse);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderPcm16BTest, EncodeDecode) {
  int tolerance = 0;
  double mse = 0.0;
  EXPECT_TRUE(CodecSupported(kDecoderPCM16B));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16Bwb));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16Bswb32kHz));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16Bswb48kHz));
  EncodeDecodeTest(2 * data_length_, tolerance, mse);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderIlbcTest, EncodeDecode) {
  int tolerance = 6808;
  double mse = 2.13e6;
  int delay = 80;  // Delay from input to output.
  EXPECT_TRUE(CodecSupported(kDecoderILBC));
  EncodeDecodeTest(500, tolerance, mse, delay);
  ReInitTest();
  EXPECT_TRUE(decoder_->HasDecodePlc());
  DecodePlcTest();
}

TEST_F(AudioDecoderIsacFloatTest, EncodeDecode) {
  int tolerance = 3399;
  double mse = 434951.0;
  int delay = 48;  // Delay from input to output.
  EXPECT_TRUE(CodecSupported(kDecoderISAC));
  EncodeDecodeTest(0, tolerance, mse, delay);
  ReInitTest();
  EXPECT_TRUE(decoder_->HasDecodePlc());
  DecodePlcTest();
}

TEST_F(AudioDecoderIsacSwbTest, EncodeDecode) {
  int tolerance = 19757;
  double mse = 8.18e6;
  int delay = 160;  // Delay from input to output.
  EXPECT_TRUE(CodecSupported(kDecoderISACswb));
  EncodeDecodeTest(0, tolerance, mse, delay);
  ReInitTest();
  EXPECT_TRUE(decoder_->HasDecodePlc());
  DecodePlcTest();
}

// Fails Android ARM64. https://code.google.com/p/webrtc/issues/detail?id=4198
#if defined(WEBRTC_ANDROID) && defined(__aarch64__)
#define MAYBE_EncodeDecode DISABLED_EncodeDecode
#else
#define MAYBE_EncodeDecode EncodeDecode
#endif
TEST_F(AudioDecoderIsacFixTest, MAYBE_EncodeDecode) {
  int tolerance = 11034;
  double mse = 3.46e6;
  int delay = 54;  // Delay from input to output.
  EXPECT_TRUE(CodecSupported(kDecoderISAC));
#ifdef WEBRTC_ANDROID
  static const int kEncodedBytes = 685;
#else
  static const int kEncodedBytes = 671;
#endif
  EncodeDecodeTest(kEncodedBytes, tolerance, mse, delay);
  ReInitTest();
  EXPECT_TRUE(decoder_->HasDecodePlc());
  DecodePlcTest();
}

TEST_F(AudioDecoderG722Test, EncodeDecode) {
  int tolerance = 6176;
  double mse = 238630.0;
  int delay = 22;  // Delay from input to output.
  EXPECT_TRUE(CodecSupported(kDecoderG722));
  EncodeDecodeTest(data_length_ / 2, tolerance, mse, delay);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderG722StereoTest, CreateAndDestroy) {
  EXPECT_TRUE(CodecSupported(kDecoderG722_2ch));
}

TEST_F(AudioDecoderG722StereoTest, EncodeDecode) {
  int tolerance = 6176;
  int channel_diff_tolerance = 0;
  double mse = 238630.0;
  int delay = 22;  // Delay from input to output.
  EXPECT_TRUE(CodecSupported(kDecoderG722_2ch));
  EncodeDecodeTest(data_length_, tolerance, mse, delay, channel_diff_tolerance);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderOpusTest, EncodeDecode) {
  int tolerance = 6176;
  double mse = 238630.0;
  int delay = 22;  // Delay from input to output.
  EXPECT_TRUE(CodecSupported(kDecoderOpus));
  EncodeDecodeTest(0, tolerance, mse, delay);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderOpusStereoTest, EncodeDecode) {
  int tolerance = 6176;
  int channel_diff_tolerance = 0;
  double mse = 238630.0;
  int delay = 22;  // Delay from input to output.
  EXPECT_TRUE(CodecSupported(kDecoderOpus_2ch));
  EncodeDecodeTest(0, tolerance, mse, delay, channel_diff_tolerance);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST(AudioDecoder, CodecSampleRateHz) {
  EXPECT_EQ(8000, CodecSampleRateHz(kDecoderPCMu));
  EXPECT_EQ(8000, CodecSampleRateHz(kDecoderPCMa));
  EXPECT_EQ(8000, CodecSampleRateHz(kDecoderPCMu_2ch));
  EXPECT_EQ(8000, CodecSampleRateHz(kDecoderPCMa_2ch));
  EXPECT_EQ(8000, CodecSampleRateHz(kDecoderILBC));
  EXPECT_EQ(16000, CodecSampleRateHz(kDecoderISAC));
  EXPECT_EQ(32000, CodecSampleRateHz(kDecoderISACswb));
  EXPECT_EQ(32000, CodecSampleRateHz(kDecoderISACfb));
  EXPECT_EQ(8000, CodecSampleRateHz(kDecoderPCM16B));
  EXPECT_EQ(16000, CodecSampleRateHz(kDecoderPCM16Bwb));
  EXPECT_EQ(32000, CodecSampleRateHz(kDecoderPCM16Bswb32kHz));
  EXPECT_EQ(48000, CodecSampleRateHz(kDecoderPCM16Bswb48kHz));
  EXPECT_EQ(8000, CodecSampleRateHz(kDecoderPCM16B_2ch));
  EXPECT_EQ(16000, CodecSampleRateHz(kDecoderPCM16Bwb_2ch));
  EXPECT_EQ(32000, CodecSampleRateHz(kDecoderPCM16Bswb32kHz_2ch));
  EXPECT_EQ(48000, CodecSampleRateHz(kDecoderPCM16Bswb48kHz_2ch));
  EXPECT_EQ(8000, CodecSampleRateHz(kDecoderPCM16B_5ch));
  EXPECT_EQ(16000, CodecSampleRateHz(kDecoderG722));
  EXPECT_EQ(16000, CodecSampleRateHz(kDecoderG722_2ch));
  EXPECT_EQ(-1, CodecSampleRateHz(kDecoderRED));
  EXPECT_EQ(-1, CodecSampleRateHz(kDecoderAVT));
  EXPECT_EQ(8000, CodecSampleRateHz(kDecoderCNGnb));
  EXPECT_EQ(16000, CodecSampleRateHz(kDecoderCNGwb));
  EXPECT_EQ(32000, CodecSampleRateHz(kDecoderCNGswb32kHz));
  EXPECT_EQ(48000, CodecSampleRateHz(kDecoderOpus));
  EXPECT_EQ(48000, CodecSampleRateHz(kDecoderOpus_2ch));
  // TODO(tlegrand): Change 32000 to 48000 below once ACM has 48 kHz support.
  EXPECT_EQ(32000, CodecSampleRateHz(kDecoderCNGswb48kHz));
  EXPECT_EQ(-1, CodecSampleRateHz(kDecoderArbitrary));
}

TEST(AudioDecoder, CodecSupported) {
  EXPECT_TRUE(CodecSupported(kDecoderPCMu));
  EXPECT_TRUE(CodecSupported(kDecoderPCMa));
  EXPECT_TRUE(CodecSupported(kDecoderPCMu_2ch));
  EXPECT_TRUE(CodecSupported(kDecoderPCMa_2ch));
  EXPECT_TRUE(CodecSupported(kDecoderILBC));
  EXPECT_TRUE(CodecSupported(kDecoderISAC));
  EXPECT_TRUE(CodecSupported(kDecoderISACswb));
  EXPECT_TRUE(CodecSupported(kDecoderISACfb));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16B));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16Bwb));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16Bswb32kHz));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16Bswb48kHz));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16B_2ch));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16Bwb_2ch));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16Bswb32kHz_2ch));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16Bswb48kHz_2ch));
  EXPECT_TRUE(CodecSupported(kDecoderPCM16B_5ch));
  EXPECT_TRUE(CodecSupported(kDecoderG722));
  EXPECT_TRUE(CodecSupported(kDecoderG722_2ch));
  EXPECT_TRUE(CodecSupported(kDecoderRED));
  EXPECT_TRUE(CodecSupported(kDecoderAVT));
  EXPECT_TRUE(CodecSupported(kDecoderCNGnb));
  EXPECT_TRUE(CodecSupported(kDecoderCNGwb));
  EXPECT_TRUE(CodecSupported(kDecoderCNGswb32kHz));
  EXPECT_TRUE(CodecSupported(kDecoderCNGswb48kHz));
  EXPECT_TRUE(CodecSupported(kDecoderArbitrary));
  EXPECT_TRUE(CodecSupported(kDecoderOpus));
  EXPECT_TRUE(CodecSupported(kDecoderOpus_2ch));
}

}  // namespace webrtc
