const streamlabs_overlays = require('../build/RelWithDebInfo/game_overlay.node'); // Ensure this line is at the top

// Constants
const LOG_FILE_PATH = "c:\\work\\over_log.log";
const INITIAL_DELAY = 2000;
const STEP_DELAY = 5000;
const SCRIPT_TIMEOUT = 25000;

class OverlayController {
    constructor(logFilePath) {
        this.logFilePath = logFilePath;
    }

    log(message) {
        const timestamp = new Date().toISOString();
        console.log(`[${timestamp}] ${message}`);
    }

    start() {
        this.log('Starting overlay...');
        streamlabs_overlays.start(this.logFilePath);
        this.log(`Overlay status: ${streamlabs_overlays.getStatus()}`);
    }

    stop() {
        this.log('Stopping overlay...');
        streamlabs_overlays.stop();
        this.log('Overlay stopped. Exiting...');
        process.exit();
    }

    scriptFinished() {
        this.log('Script finished.');
        this.stop();
    }

    stepFinish() {
        this.log('Step finished.');
        setTimeout(() => this.scriptFinished(), INITIAL_DELAY);
    }

    step4() {
        this.log('Executing step 4: Disabling input collection again.');
        streamlabs_overlays.switchInputCollection(false);
        setTimeout(() => this.stepFinish(), STEP_DELAY);
    }

    step3() {
        this.log('Executing step 3: Re-enabling input collection.');
        streamlabs_overlays.switchInputCollection(true);
        setTimeout(() => this.step4(), STEP_DELAY);
    }

    step2() {
        this.log('Executing step 2: Enabling input collection.');
        streamlabs_overlays.switchInputCollection(true);
        setTimeout(() => this.step3(), STEP_DELAY);
    }

    step1() {
        this.log('Executing step 1: Setting up input callbacks.');

        streamlabs_overlays.setMouseCallback((eventType, x, y, modifier) => {
            this.log(`MouseCallback: eventType=${eventType}, x=${x}, y=${y}, modifier=${modifier}`);
            return 3;
        });

        streamlabs_overlays.setKeyboardCallback((eventType, keyCode) => {
            this.log(`KeyboardCallback: eventType=${eventType}, keyCode=${keyCode}`);
            if (keyCode === 38) {
                streamlabs_overlays.switchInputCollection(false);
            }
            return 1;
        });

        setTimeout(() => this.step2(), STEP_DELAY);
    }

    run() {
        try {
            setTimeout(() => this.step1(), INITIAL_DELAY);
            setTimeout(() => this.scriptFinished(), SCRIPT_TIMEOUT);
        } catch (error) {
            this.log(`An error occurred: ${error}`);
            this.scriptFinished();
        }
    }
}

// Instantiate and run the overlay controller
const overlayController = new OverlayController(LOG_FILE_PATH);
overlayController.start();
overlayController.run();