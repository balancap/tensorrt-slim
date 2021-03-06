/* ============================================================================
# [2017] - Robik AI Ltd - Paul Balanca
# All Rights Reserved.

# NOTICE: All information contained herein is, and remains
# the property of Robik AI Ltd, and its suppliers
# if any.  The intellectual and technical concepts contained
# herein are proprietary to Robik AI Ltd
# and its suppliers and may be covered by U.S., European and Foreign Patents,
# patents in process, and are protected by trade secret or copyright law.
# Dissemination of this information or reproduction of this material
# is strictly forbidden unless prior written permission is obtained
# from Robik AI Ltd.
# =========================================================================== */
#include <glog/logging.h>
#include <gflags/gflags.h>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <memory>

#include <NVX/nvx.h>
#include <NVX/nvx_timer.hpp>

#include <NVX/Application.hpp>
#include <OVX/FrameSourceOVX.hpp>
#include <OVX/RenderOVX.hpp>
#include <NVX/SyncTimer.hpp>
#include <OVX/UtilityOVX.hpp>

#include <stabilization/stabilizer.hpp>

// ROS headers
#include <sensor_msgs/Image.h>

#include <tensorflowrt.h>
#include <tensorflowrt_util.h>
#include <tensorflowrt_models.h>

#define DEMONET "<demo-single-input> "

/* ============================================================================
 * Demo flags.
 * ========================================================================== */
// Source parameters.
DEFINE_string(source, "../data/parking.avi", "Video source URI, webcam camera or video file.");
DEFINE_int32(source_width, 1280, "Source width. Only for camera.");
DEFINE_int32(source_height, 720, "Source height. Only for camera.");
DEFINE_int32(source_fps, 60, "Source fps. Only for camera.");

// Network parameters.
DEFINE_string(net_name, "ssd_inception2_v0", "SSD network network to use.");
DEFINE_string(net_checkpoint, "../data/networks/ssd_inception2_v0_orig.tfrt32",
    "Network protobuf checkpoint file.");
DEFINE_int32(net_height, 225, "Network input height.");
DEFINE_int32(net_width, 400, "Network input width.");

// Display parameters.
DEFINE_bool(display_scale, true, "Scale display?");
DEFINE_bool(display_fullscreen, true, "Fullscreen display?");


// DEFINE_bool(image_save, false, "Save the result in some new image.");
// DEFINE_double(threshold, 0.5, "Detection threshold.");

/* ============================================================================
 * (simple )Event management: pause or quit!
 * ========================================================================== */
struct EventData
{
    EventData(): shouldStop(false), pause(false) {}
    bool shouldStop;
    bool pause;
};
static void eventCallback(void* eventData, vx_char key, vx_uint32, vx_uint32)
{
    EventData* data = static_cast<EventData*>(eventData);
    if (key == 27) {
        data->shouldStop = true;
    }
    else if (key == 32) {
        data->pause = !data->pause;
    }
}

/* ============================================================================
 * sub-routines: create frame source, ...
 * ========================================================================== */
static std::unique_ptr<ovxio::FrameSource> create_frame_source(const ovxio::ContextGuard& context)
{
    LOG(INFO) << DEMONET << "Opening frame source: " << FLAGS_source;
    // Get default frame source.
    ovxio::FrameSource::Parameters sourceParams;
    std::unique_ptr<ovxio::FrameSource> source(
        ovxio::createDefaultFrameSource(context, FLAGS_source));
    CHECK(source) << DEMONET << "ERROR: can't open source: " << FLAGS_source;
    // Set the parameters for camera source.
    if(source->getSourceType() == ovxio::FrameSource::CAMERA_SOURCE) {
        LOG(INFO) << DEMONET << "Setting frame source parameters.";
        sourceParams = source->getConfiguration();
        sourceParams.frameHeight = FLAGS_source_height;
        sourceParams.frameWidth = FLAGS_source_width;
        sourceParams.fps = FLAGS_source_fps;
        // sourceParams.format = NVXCU_DF_IMAGE_RGBX;
        source->setConfiguration(sourceParams);
    }
    CHECK(source->open()) << DEMONET << "ERROR: can't open source: " << FLAGS_source;
    CHECK(source->getSourceType() != ovxio::FrameSource::SINGLE_IMAGE_SOURCE)
        << DEMONET << "ERROR: Can't work on a single image.";
    // Debug...
    sourceParams = source->getConfiguration();
    LOG(INFO) << "Frame source opened: " << sourceParams.frameWidth << "x"
        << sourceParams.frameHeight << "@" << sourceParams.fps;
    return source;
}

/* ============================================================================
 * Display information on the screen.
 * ========================================================================== */
/** Create the input render. */
static std::unique_ptr<ovxio::Render> create_input_render(
    const ovxio::ContextGuard& context)
{
    vx_int32 height = FLAGS_source_height;
    vx_int32 width = FLAGS_source_width;
    std::unique_ptr<ovxio::Render> renderer(ovxio::createDefaultRender(
        context, "Video Stabilization Demo: inputs", width, height,
            VX_DF_IMAGE_RGBX, false, false));
    CHECK(renderer) << DEMONET << "ERROR: Can't create a input renderer.";
    return renderer;
}
/** Get the input render image to display. */
static vx_image get_input_render_image(const ovxio::ContextGuard& context, vx_image frame)
{
    static vx_image input_buffer_img{nullptr};
    // Create static object.
    if (input_buffer_img == nullptr) {
        input_buffer_img = vxCreateImage(
            context, FLAGS_source_width, FLAGS_source_height, VX_DF_IMAGE_RGBX);
        NVXIO_CHECK_REFERENCE(input_buffer_img);
    }
    // Copy image to render buffer.
    if (frame) {
        NVXIO_SAFE_CALL( nvxuCopyImage(context, frame, input_buffer_img) );
    }
    return input_buffer_img;
}

/** Create the display render. */
static std::unique_ptr<ovxio::Render> create_display_render(
    const ovxio::ContextGuard& context)
{
    vx_int32 height = FLAGS_net_height;
    vx_int32 width = FLAGS_net_width;
    std::unique_ptr<ovxio::Render> renderer(ovxio::createDefaultRender(
        context, "Video Stabilization Demo", width, height,
            VX_DF_IMAGE_RGBX, FLAGS_display_scale, FLAGS_display_fullscreen));
    CHECK(renderer) << DEMONET << "ERROR: Can't create a display renderer.";
    return renderer;
}
/** Get the input render image to display. */
static vx_image get_display_render_image(const ovxio::ContextGuard& context, vx_image stab_frame)
{
    vx_int32 height = FLAGS_net_height;
    vx_int32 width = FLAGS_net_width;
    static vx_image display_buffer_img{nullptr};
    // Create static object.
    if (display_buffer_img == nullptr) {
        display_buffer_img = vxCreateImage(context, width, height, VX_DF_IMAGE_RGBX);
        NVXIO_CHECK_REFERENCE(display_buffer_img);
    }
    // Copy image to render buffer.
    if (stab_frame) {
        NVXIO_SAFE_CALL( nvxuCopyImage(context, stab_frame, display_buffer_img) );
    }
    return display_buffer_img;
}

static void update_display(
    ovxio::Render *renderer, const ovxio::FrameSource::Parameters& sourceParams,
    double proc_ms, double total_ms, float cropMargin)
{
    vx_uint32 renderWidth = renderer->getViewportWidth();

    std::ostringstream txt;
    txt << std::fixed << std::setprecision(1);

    const vx_int32 borderSize = 10;
    ovxio::Render::TextBoxStyle style = {{255, 255, 255, 255}, {0, 0, 0, 127}, {renderWidth / 2 + borderSize, borderSize}};

    txt << "Input: " << sourceParams.frameWidth << 'x' << sourceParams.frameHeight << " | " << sourceParams.fps << " | " << sourceParams.format << std::endl;
    txt << "Algorithm: " << proc_ms << " ms / " << 1000.0 / proc_ms << " FPS" << std::endl;
    txt << "Display: " << total_ms  << " ms / " << 1000.0 / total_ms << " FPS" << std::endl;

    txt << std::setprecision(6);
    txt.unsetf(std::ios_base::floatfield);
    txt << "LIMITED TO " << nvxio::Application::get().getFPSLimit() << " FPS FOR DISPLAY" << std::endl;
    txt << "Space - pause/resume" << std::endl;
    txt << "Esc - close the demo";
    renderer->putTextViewport(txt.str(), style);

    const vx_int32 stabilizedLabelLenght = 100;
    style.origin.x = renderWidth - stabilizedLabelLenght;
    style.origin.y = borderSize;
    renderer->putTextViewport("stabilized", style);

    style.origin.x = renderWidth / 2 - stabilizedLabelLenght;
    renderer->putTextViewport("original", style);

    if (cropMargin > 0)
    {
        vx_uint32 dx = static_cast<vx_uint32>(cropMargin * sourceParams.frameWidth);
        vx_uint32 dy = static_cast<vx_uint32>(cropMargin * sourceParams.frameHeight);
        vx_rectangle_t rect = {dx, dy, sourceParams.frameWidth - dx, sourceParams.frameHeight - dy};

        ovxio::Render::DetectedObjectStyle rectStyle = {{""}, {255, 255, 255, 255}, 2, 0, false};
        renderer->putObjectLocation(rect, rectStyle);
    }
}

/* ============================================================================
 * Main entry point...
 * ========================================================================== */
int main(int argc, char* argv[])
{
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    try
    {
        nvxio::Application &app = nvxio::Application::get();
        ovxio::printVersionInfo();
        app.setDescription("Demo of video stabilization + SSD neural net.");
        app.init(argc, argv);

        /* ============================================================================
         * Create OpenVX context, frame source and render.
         * ========================================================================== */
        // Performance config. TODO: enable logging too?
        ovxio::ContextGuard context;
        vxRegisterLogCallback(context, &ovxio::stdoutLogCallback, vx_false_e);
        vxDirective(context, VX_DIRECTIVE_ENABLE_PERFORMANCE);

        // Create the default frame source on URI.
        auto source = create_frame_source(context);
        auto sourceParams = source->getConfiguration();

        // Input rendering window.
        auto input_renderer = create_input_render(context);
        vx_image input_buffer_img{nullptr};
        // Create the output render window.
        auto display_renderer = create_display_render(context);
        vx_image display_buffer_img{nullptr};
        EventData eventData;
        display_renderer->setOnKeyboardEventCallback(eventCallback, &eventData);

        // Stabilization parameters + update neural net shape.
        nvx::VideoStabilizer::VideoStabilizerParams params;
        params.output_height = FLAGS_net_height;
        params.output_width = FLAGS_net_width;
        params.crop_scale_y = float(params.output_height) / (FLAGS_source_height * (1. - params.crop_y));
        params.crop_scale_x = float(params.output_width) / (FLAGS_source_width * (1. - params.crop_x));
        // params.crop_scale_y = std::min(params.crop_scale_y, params.crop_scale_x);
        // params.crop_scale_x = std::min(params.crop_scale_y, params.crop_scale_x);

        /* ============================================================================
         * Stack of frames.
         * ========================================================================== */
        vx_image frameExemplar = vxCreateImage(
            context, sourceParams.frameWidth, sourceParams.frameHeight, VX_DF_IMAGE_RGBX);
        // Must have such size to be synchronized with the stabilized frames.
        vx_size orig_frame_delay_size = params.num_smoothing_frames + 2;
        vx_delay orig_frame_delay = vxCreateDelay(
            context, (vx_reference)frameExemplar, orig_frame_delay_size);
        NVXIO_CHECK_REFERENCE(orig_frame_delay);
        NVXIO_SAFE_CALL(nvx::initDelayOfImages(context, orig_frame_delay));
        NVXIO_SAFE_CALL(vxReleaseImage(&frameExemplar));
        // References to first and last in the stack.
        vx_image frame = (vx_image)vxGetReferenceFromDelay(orig_frame_delay, 0);
        vx_image last_frame = (vx_image)vxGetReferenceFromDelay(
            orig_frame_delay, 1 - static_cast<vx_int32>(orig_frame_delay_size));

        /* ============================================================================
         * Create VideoStabilizer instance
         * ========================================================================== */
        auto stabilizer = nvx::VideoStabilizer::createImageBasedVStab(context, params);
        // Get rid of timeout frames + check source not closed.
        ovxio::FrameSource::FrameStatus frameStatus;
        do {
            frameStatus = source->fetch(frame);
        }
        while (frameStatus == ovxio::FrameSource::TIMEOUT);
        if (frameStatus == ovxio::FrameSource::CLOSED) {
            std::cerr << "Error: Source has no frames" << std::endl;
            return nvxio::Application::APP_EXIT_CODE_NO_FRAMESOURCE;
        }
        // A few initial checks and initialization on input frame.
        stabilizer->init(frame);

        // // Left and right sub-images...
        // vx_rectangle_t leftRect;
        // NVXIO_SAFE_CALL( vxGetValidRegionImage(frame, &leftRect) );
        // vx_rectangle_t rightRect;
        // rightRect.start_x = leftRect.end_x;
        // rightRect.start_y = leftRect.start_y;
        // rightRect.end_x = 2 * leftRect.end_x;
        // rightRect.end_y = leftRect.end_y;

        // vx_image leftRoi = vxCreateImageFromROI(demoImg, &leftRect);
        // NVXIO_CHECK_REFERENCE(leftRoi);
        // vx_image rightRoi = vxCreateImageFromROI(demoImg, &rightRect);
        // NVXIO_CHECK_REFERENCE(rightRoi);

        /* ============================================================================
         * Run processing loop
         * ========================================================================== */
        std::unique_ptr<nvxio::SyncTimer> syncTimer = nvxio::createSyncTimer();
        syncTimer->arm(1. / app.getFPSLimit());
        std::cout << "Application FPS: " << app.getFPSLimit() << std::endl;

        nvx::Timer totalTimer;
        totalTimer.tic();
        double proc_ms = 0;

        while (!eventData.shouldStop) {
            if (!eventData.pause) {
                // Stabilize inputs.
                nvx::Timer procTimer;
                procTimer.tic();
                stabilizer->process(frame);
                proc_ms = procTimer.toc();
                // Delay frame stack and get result.
                NVXIO_SAFE_CALL( vxAgeDelay(orig_frame_delay) );
                vx_image stab_frame = stabilizer->get_frame_stabilized();
                // Update rendering buffer images.
                input_buffer_img = get_input_render_image(context, last_frame);
                display_buffer_img = get_display_render_image(context, stab_frame);

                // Print performance results
                stabilizer->print_performances();
                // Read next frame. SLOW???
                frameStatus = source->fetch(frame, 1);
                double fetch_ms = totalTimer.toc();
                std::cout << "Frame fetch time : " << fetch_ms << " ms" << std::endl;
                if (frameStatus == ovxio::FrameSource::TIMEOUT) {
                    std::cout << "Frame timeout..." << std::endl;
                    continue;
                }
                else if (frameStatus == ovxio::FrameSource::CLOSED) {
                    if (!source->open()) {
                        std::cerr << "Error: Failed to reopen the source" << std::endl;
                        break;
                    }
                }
            }
            // Push buffer images to renderers.
            input_renderer->putImage(input_buffer_img);
            display_renderer->putImage(display_buffer_img);

            double total_ms = totalTimer.toc();
            std::cout << "Display Time : " << total_ms << " ms" << std::endl << std::endl;

            syncTimer->synchronize();
            total_ms = totalTimer.toc();
            totalTimer.tic();
            // update_display(renderer.get(), sourceParams, proc_ms, total_ms, params.crop_margin);

            // Flush rendering windows.
            if (!input_renderer->flush() || !display_renderer->flush()) {
                eventData.shouldStop = true;
            }
        }

        // Release all objects
        // renderer->close();
        // vxReleaseImage(&demoImg);
        // vxReleaseImage(&leftRoi);
        // vxReleaseImage(&rightRoi);
        vxReleaseDelay(&orig_frame_delay);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return nvxio::Application::APP_EXIT_CODE_ERROR;
    }
    return nvxio::Application::APP_EXIT_CODE_SUCCESS;
}
