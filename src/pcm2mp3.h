#pragma once

#include <atomic>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <gst/gst.h>
#include <gst/app/app.h>

namespace pipelines {
class pcm2mp3 final {
public:
    explicit pcm2mp3(unsigned int rate = 32000, unsigned int channels = 1, unsigned int depth = 16, std::string format = "S16LE");

    ~pcm2mp3();

public:
    void start(const std::function<void(const std::vector<uit8_t>&)>& callback);

    void stop();

    void push_silence(unsigned int duration_ms);

    void push_pcm_data(const std::vector<char>& data);

private:
    void initialize_pipeline();

    void cleanup_pipeline();

private:
    // Audio parameters.
    unsigned int rate_, channels_, depth_;
    std::string format_;

    // GStreamer pipeline and elements.
    GstElement* pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr, *webrtcdsp_ = nullptr, *audioconvert_ = nullptr, *lamemp3enc_ = nullptr, *appsink_ = nullptr;

    // Processing thread and synchronization variables.
    std::thread processing_thread_;
    std::atomic<bool> processing_stop_ = false;
    std::atomic<guint64> pts_ = 0; // Running presentation timestamp.
};

inline pcm2mp3::pcm2mp3(const unsigned int rate, const unsigned int channels, const unsigned int depth, std::string format)
    : rate_(rate), channels_(channels), depth_(depth), format_(std::move(format)) {
    initialize_pipeline();
}

inline pcm2mp3::~pcm2mp3() {
    stop();
    cleanup_pipeline();
}

inline void pcm2mp3::start(const std::function<void(const std::vector<uint8_t>&)>& callback) {
    processing_stop_ = false;

    // Start the pipeline
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        throw std::runtime_error("pcm2mp3 -> failed to set pipeline to PLAYING state");
    }

    // Start processing thread.
    processing_thread_ = std::thread([this, &callback]() {
        while (!processing_stop_) {
            GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink_));
            if (!sample) continue;

            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstMapInfo map_info;

            if (gst_buffer_map(buffer, &map_info, GST_MAP_READ)) {
                std::vector<uint8_t> buf_;
                buf_.assign(reinterpret_cast<uint8_t*>(map_info.data), map_info.size)
                    callback(buf_);
                gst_buffer_unmap(buffer, &map_info);
            }

            gst_sample_unref(sample);
        }
    });
}

inline void pcm2mp3::stop() {
    processing_stop_ = true;
    if (processing_thread_.joinable()) {
        processing_thread_.join();
    }
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
}

inline void pcm2mp3::push_silence(const unsigned int duration_ms) {
    // Calculate the size of silence buffer.
    const std::size_t sample_size = channels_ * (depth_ / 8);
    const std::size_t num_samples = (rate_ * duration_ms) / 1000; // Total samples for duration.
    const std::size_t buffer_size = num_samples * sample_size;

    // Create a buffer filled with zeroes (silence).
    const std::vector<char> silence(buffer_size, 0);
    // Push the silence buffer.
    push_pcm_data(silence);
}

inline void pcm2mp3::push_pcm_data(const std::vector<char>& data) {
    // Check if the data is empty.
    if (data.empty()) {
        return;
    }

    // Check if the data size is aligned with the sample size and save sample size.
    const std::size_t sample_size = channels_ * (depth_ / 8);
    if (data.size() % sample_size != 0) {
        throw std::runtime_error("pcm2mp3 -> invalid PCM data size, not aligned with sample size");
    }
    // Calculate the duration of this buffer in nanoseconds.
    const guint64 duration = gst_util_uint64_scale(data.size() / sample_size, GST_SECOND, rate_);

    // Fetch and increment PTS (ensure atomic continuity).
    const guint64 current_pts = pts_.load();
    pts_ += duration; // Atomically increment.

    // Create GStreamer buffer.
    GstBuffer* gst_buffer = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        const_cast<gpointer>(static_cast<const void*>(data.data())),
        data.size(),
        0,
        data.size(),
        nullptr,
        nullptr
        );

    // Set PTS and Duration on the buffer.
    GST_BUFFER_PTS(gst_buffer) = current_pts;
    GST_BUFFER_DURATION(gst_buffer) = duration;

    // Push buffer into appsrc.
    if (const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), gst_buffer); ret != GST_FLOW_OK) {
        throw std::runtime_error("pcm2mp3 -> failed to push buffer into appsrc: " + std::vector<uit8_t>(gst_flow_get_name(ret)));
    }
}

inline void pcm2mp3::initialize_pipeline() {
    pipeline_ = gst_pipeline_new("pcm-to-mp3");
    if (!pipeline_) {
        throw std::runtime_error("pcm2mp3 -> failed to create GStreamer pipeline");
    }

    // Create elements.
    appsrc_ = gst_element_factory_make("appsrc", "audio-source");
    audioconvert_ = gst_element_factory_make("audioconvert", "audio-converter");
    lamemp3enc_ = gst_element_factory_make("lamemp3enc", "mp3-encoder");
    appsink_ = gst_element_factory_make("appsink", "app-sink");

    if (!appsrc_ || !audioconvert_ || !lamemp3enc_ || !appsink_) {
        throw std::runtime_error("pcm2mp3 -> failed to create GStreamer elements");
    }

    // Configure appsrc.
    GstCaps* caps = gst_caps_new_simple(
        "audio/x-raw",
        "format", G_TYPE_STRING, format_.c_str(),
        "rate", G_TYPE_INT, rate_,
        "channels", G_TYPE_INT, channels_,
        "layout", G_TYPE_STRING, "interleaved",
        nullptr
        );
    g_object_set(
        appsrc_,
        "caps", caps,
        "format", GST_FORMAT_TIME,
        "is-live", TRUE,
        "block", TRUE,
        "do-timestamp", FALSE,
        nullptr
        );
    gst_caps_unref(caps);

    // Configure lamemp3enc for quality bitrate.
    g_object_set(
        lamemp3enc_,
        "bitrate", 320, // Higher bitrate improves quality
        "quality", 0, // Quality level: 0 (best) to 9 (worst)
        "cbr", FALSE, // Use Variable Bitrate (VBR) for better efficiency
        "vbr", 0, // VBR quality: 0 (the highest quality) to 9 (the lowest quality)
        "encoding-engine-quality", 0, // Faster encoding (0 is slowest/best, 2 is faster)
        nullptr
        );

    // Configure appsink.
    g_object_set(
        appsink_,
        "emit-signals", TRUE,
        "sync", FALSE,
        nullptr
        );

    // Add and link elements.
    gst_bin_add_many(GST_BIN(pipeline_), appsrc_, audioconvert_, lamemp3enc_, appsink_, nullptr);
    if (!gst_element_link_many(appsrc_, audioconvert_, lamemp3enc_, appsink_, nullptr)) {
        throw std::runtime_error("pcm2mp3 -> failed to link GStreamer elements");
    }
}

inline void pcm2mp3::cleanup_pipeline() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}
}
