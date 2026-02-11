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

    // Open the GPIO chip.
    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    if (chip == NULL) {
        // TODO Show the user some error.
        return;
    }

    // Configure the line settings on pin 17.
    unsigned int offset = 17;
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);

    // Optional but highly recommended: If your physical button doesn't have an external 
    // resistor, uncomment one of these to use the Pi's internal resistors so the pin doesn't float!
    // gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_DOWN); // If button connects to 3.3V
    // gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);   // If button connects to Ground

    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);

    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(req_cfg, "rpicam-button");

    // Request the line.
    struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (request == NULL) {
        // TODO Show the user some error.
        return;
    }

    // Free the config structs (they are no longer needed after the request is made)
    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);

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
            // Read the current value of the pin
            enum gpiod_line_value shutter_button_value = gpiod_line_request_get_value(request, offset);

            bool timeout_passed = options->Get().timeout && (now - start_time) > options->Get().timeout.value;
            bool key_pressed = key == 'c';
            bool shutter_button_pressed = shutter_button_value == GPIOD_LINE_VALUE_ACTIVE;

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

    gpiod_line_request_release(request);
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
