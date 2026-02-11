/*
 * mathorcam.cpp - minimal libcamera jpeg capture app.
 */

#include <chrono>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
// #include <gpiod.h>

#include "core/rpicam_app.hpp"
#include "core/still_options.hpp"

#include "output/output.hpp"

#include "image/image.hpp"

using namespace std::placeholders;
using libcamera::Stream;

class MathorcamApp : public RPiCamApp {
public:
    MathorcamApp() : RPiCamApp(std::make_unique<StillOptions>()) {
    }

    StillOptions *GetOptions() const {
        return static_cast<StillOptions *>(RPiCamApp::GetOptions());
    }
};

// Save metadata to file
static void save_metadata(StillOptions const *options, libcamera::ControlList &metadata) {
    std::streambuf *buf = std::cout.rdbuf();
    std::ofstream of;
    const std::string &filename = options->Get().metadata;

    if (filename.compare("-")) {
        of.open(filename, std::ios::out);
        buf = of.rdbuf();
    }

    write_metadata(buf, options->Get().metadata_format, metadata, true);
}

int kbhit() {
    struct termios oldt, newt;
    int ch;
    int oldf;

    // Get current terminal settings
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    
    // Disable canonical mode (don't wait for Enter) and echo
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    
    // Set standard input to non-blocking
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    ch = getchar();

    // Restore original terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);

    if(ch != EOF) {
        return ch; // Return the key that was pressed
    }
    return 0; // Return 0 if no key was pressed
}

// The main loop for the application.
static void event_loop(MathorcamApp& app) {
    StillOptions const *options = app.GetOptions();
    app.OpenCamera();
    app.ConfigureViewfinder();
    app.StartCamera();
    std::__1::chrono::steady_clock::time_point start_time = std::chrono::high_resolution_clock::now();

    // Fetch GPIO pin reference.
    // gpiod_chip *chip = gpiod_chip_open_by_name("gpiochip0"); // RPi 5 uses gpiochip4 usually, RPi 4 uses gpiochip0
    // gpiod_line *line = gpiod_chip_get_line(chip, 17); // GPIO Pin 17

    for (;;) {
		// Read RPiCamApp message.
        RPiCamApp::Msg msg = app.Wait();

        if (msg.type == RPiCamApp::MsgType::Timeout) {
            LOG_ERROR("ERROR: Device timeout detected, attempting a restart!!!");
            app.StopCamera();
            app.StartCamera();
            continue;
        }

        if (msg.type == RPiCamApp::MsgType::Quit)
            break;

        else if (msg.type != RPiCamApp::MsgType::RequestComplete)
            // This is critical since GPIO chip is not closed.
            throw std::runtime_error("unrecognised message!");

        // In viewfinder mode, simply run until the timeout. When that happens, switch to
        // capture mode.
        if (app.ViewfinderStream()) {
			// Read the current time from chrono.
            std::__1::chrono::steady_clock::time_point now = std::chrono::high_resolution_clock::now();

            // Fetch any pressed key.
            int key = kbhit();

            bool timeout_passed = options->Get().timeout && (now - start_time) > options->Get().timeout.value;
            bool shutter_button_pressed = false;//gpiod_line_get_value(line) == 1;
            bool key_pressed = key == 'c';

            if (timeout_passed || key_pressed || shutter_button_pressed) {
				// Change mode to still picture.
                app.StopCamera();
                app.Teardown();
                app.ConfigureStill();
                app.StartCamera();
            } else {
                CompletedRequestPtr &completed_request = std::get<CompletedRequestPtr>(msg.payload);
                app.ShowPreview(completed_request, app.ViewfinderStream());
            }
        } else if (app.StillStream()) {
			// In still capture mode, save a jpeg and get back to viewfinder mode.
            app.StopCamera();
            LOG(1, "Still capture image received");

            Stream *stream = app.StillStream();
            StreamInfo info = app.GetStreamInfo(stream);
            CompletedRequestPtr &payload = std::get<CompletedRequestPtr>(msg.payload);
            BufferReadSync r(&app, payload->buffers[stream]);
            const std::vector<libcamera::Span<uint8_t>> mem = r.Get();

            // TODO Fetch target output file.
            std::string output_file_path = options->Get().output;

            jpeg_save(
                mem,
                info,
                payload->metadata,
                output_file_path,
                app.CameraModel(),
                options
            );

            if (!options->Get().metadata.empty())
                save_metadata(options, payload->metadata);

			// Get back to viewfinder mode.
			app.StopCamera();
			app.Teardown();
			app.ConfigureViewfinder();
			app.StartCamera();
        }
    }

    gpiod_chip_close(chip);
}

int main(int argc, char *argv[]) {
    try {
        MathorcamApp app;
        StillOptions *options = app.GetOptions();
        if (options->Parse(argc, argv)) {
            if (options->Get().verbose >= 2)
                options->Get().Print();
            if (options->Get().output.empty())
                throw std::runtime_error("output file name required");

            if (options->GetPlatform() == Platform::PISP) {
                LOG_ERROR("WARNING: Capture will not make use of temporal denoise");
                LOG_ERROR("         Consider using rpicam-still with the --zsl option for best results, for example:");
                LOG_ERROR("         rpicam-still --zsl -o " << options->Get().output);
            }

            event_loop(app);
        }
    } catch (std::exception const &e) {
        LOG_ERROR("ERROR: *** " << e.what() << " ***");
        return -1;
    }
    return 0;
}
