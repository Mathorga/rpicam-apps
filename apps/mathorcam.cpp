/*
 * mathorcam.cpp - minimal libcamera jpeg capture app.
 */

#include <chrono>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <gpiod.h>

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

// Call this ONCE before your loop
void setup_keyboard(struct termios orig_termios) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    
    // Disable canonical mode (waiting for Enter) and echoing (printing the key)
    raw.c_lflag &= ~(ICANON | ECHO); 
    
    // Set read() to be completely non-blocking
    raw.c_cc[VMIN] = 0;  // Minimum characters to wait for
    raw.c_cc[VTIME] = 0; // Timeout in deciseconds
    
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

// Call this ONCE after your loop finishes
void restore_keyboard(struct termios orig_termios) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

// The new, much simpler kbhit
int get_keypress() {
    char ch = 0;
    // read() will instantly return 0 if no key is pressed, or 1 if a key is pressed
    if (read(STDIN_FILENO, &ch, 1) > 0) {
        return ch;
    }
    return 0;
}

// The main loop for the application.
static void event_loop(MathorcamApp& app) {
    StillOptions const *options = app.GetOptions();
    app.OpenCamera();
    app.ConfigureViewfinder();
    app.StartCamera();
    auto start_time = std::chrono::high_resolution_clock::now();

    // Fetch GPIO pin reference.
    gpiod_chip *chip = gpiod_chip_open_by_name("gpiochip0"); // RPi 5 uses gpiochip4 usually, RPi 4 uses gpiochip0
    gpiod_line *line = gpiod_chip_get_line(chip, 17); // GPIO Pin 17

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
		auto now = std::chrono::high_resolution_clock::now();

            // Fetch any pressed key.
            int key = get_keypress();

            bool timeout_passed = options->Get().timeout && (now - start_time) > options->Get().timeout.value;
            bool key_pressed = key == 'c';
            bool shutter_button_pressed = gpiod_line_get_value(line) == 1;

            if (key != 0)
                printf("\n%c - %d\n", key, key);

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
    // Store the original terminal settings so we can restore them later
    struct termios orig_termios;
    setup_keyboard(orig_termios);

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
        restore_keyboard(orig_termios);
        return -1;
    }
    restore_keyboard(orig_termios);
    return 0;
}
