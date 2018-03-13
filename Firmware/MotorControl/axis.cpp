
#include <stdlib.h>
#include <functional>
#include "gpio.h"

#include "utils.h"
#include "odrive_main.hpp"

Axis::Axis(const AxisHardwareConfig_t& hw_config,
           AxisConfig_t& config,
           Encoder& encoder,
           SensorlessEstimator& sensorless_estimator,
           Controller& controller,
           Motor& motor)
    : hw_config_(hw_config),
      config_(config),
      encoder_(encoder),
      sensorless_estimator_(sensorless_estimator),
      controller_(controller),
      motor_(motor)
{
    encoder_.axis_ = this;
    sensorless_estimator_.axis_ = this;
    controller_.axis_ = this;
    motor_.axis_ = this;
}

// @brief Sets up all components of the axis,
// such as gate driver and encoder hardware.
void Axis::setup() {
    encoder_.setup();
    motor_.setup();
}

static void run_state_machine_loop_wrapper(void* ctx) {
    reinterpret_cast<Axis*>(ctx)->run_state_machine_loop();
}

// @brief Starts run_state_machine_loop in a new thread
void Axis::start_thread() {
    osThreadDef(thread_def, run_state_machine_loop_wrapper, hw_config_.thread_priority, 0, 4*512);
    thread_id_ = osThreadCreate(osThread(thread_def), this);
    thread_id_valid_ = true;
}

// @brief Unblocks the control loop thread.
// This is called from the current sense interrupt handler.
void Axis::signal_current_meas() {
    if (thread_id_valid_)
        osSignalSet(thread_id_, M_SIGNAL_PH_CURRENT_MEAS);
}

// @brief Blocks until a current measurement is completed
// @returns True on success, false otherwise
bool Axis::wait_for_current_meas() {
    if (osSignalWait(M_SIGNAL_PH_CURRENT_MEAS, PH_CURRENT_MEAS_TIMEOUT).status != osEventSignal)
        return error_ = ERROR_CURRENT_MEASUREMENT_TIMEOUT, false;
    return true;
}

static void step_cb_wrapper(void* ctx) {
    reinterpret_cast<Axis*>(ctx)->step_cb();
}

// step/direction interface
void Axis::step_cb() {
    if (enable_step_dir_) {
        GPIO_PinState dir_pin = HAL_GPIO_ReadPin(hw_config_.dir_port, hw_config_.dir_pin);
        float dir = (dir_pin == GPIO_PIN_SET) ? 1.0f : -1.0f;
        controller_.pos_setpoint_ += dir * config_.counts_per_step;
    }
};

// @brief Enables or disables step/dir input
void Axis::set_step_dir_enabled(bool enable) {
    if (enable) {
        // Set up the direction GPIO as input
        GPIO_InitTypeDef GPIO_InitStruct;
        GPIO_InitStruct.Pin = hw_config_.dir_pin;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(hw_config_.dir_port, &GPIO_InitStruct);

        // Subscribe to rising edges of the step GPIO
        GPIO_subscribe(hw_config_.step_port, hw_config_.step_pin, GPIO_PULLDOWN,
                step_cb_wrapper, this);

        enable_step_dir_ = true;
    } else {
        enable_step_dir_ = false;

        // Unsubscribe from step GPIO
        GPIO_unsubscribe(hw_config_.step_port, hw_config_.step_pin);
    }
}

// @brief Returns true if the power supply is within range
bool Axis::check_PSU_brownout() {
    return vbus_voltage >= config_.dc_bus_brownout_trip_level;
}

// @brief Returns true if everything is ok.
// Sets error and returns false otherwise.
bool Axis::do_checks() {
    if (!motor_.do_checks())
        return error_ = ERROR_MOTOR_FAILED, false;
    if (!check_PSU_brownout())
        return error_ = ERROR_DC_BUS_UNDER_VOLTAGE, false;
    return true;
}

bool Axis::run_sensorless_spin_up() {
    // Early Spin-up: spiral up current
    float x = 0.0f;
    run_control_loop([&](){
        float phase = wrap_pm_pi(config_.ramp_up_distance * x);
        float I_mag = config_.spin_up_current * x;
        x += current_meas_period / config_.ramp_up_time;
        if (!motor_.update(I_mag, phase))
            return error_ = ERROR_MOTOR_FAILED, false;
        return x < 1.0f;
    });
    if (error_ != ERROR_NO_ERROR)
        return false;
    
    // Late Spin-up: accelerate
    float vel = config_.ramp_up_distance / config_.ramp_up_time;
    float phase = wrap_pm_pi(config_.ramp_up_distance);
    run_control_loop([&](){
        vel += config_.spin_up_acceleration * current_meas_period;
        phase = wrap_pm_pi(phase + vel * current_meas_period);
        float I_mag = config_.spin_up_current;
        if (!motor_.update(I_mag, phase))
            return error_ = ERROR_MOTOR_FAILED, false;
        return vel < config_.spin_up_target_vel;
    });
    return error_ == ERROR_NO_ERROR;
}

// Note run_sensorless_control_loop and run_closed_loop_control_loop are very similar and differ only in where we get the estimate from.
bool Axis::run_sensorless_control_loop() {
    set_step_dir_enabled(config_.enable_step_dir);
    run_control_loop([this](){
        float pos_estimate, vel_estimate, phase, current_setpoint;

        if (controller_.config_.control_mode >= CTRL_MODE_POSITION_CONTROL)
            return error_ = ERROR_POS_CTRL_DURING_SENSORLESS, false;

        // We update the encoder just in case someone needs the output for testing
        encoder_.update(nullptr, nullptr, nullptr);
        if (!sensorless_estimator_.update(&pos_estimate, &vel_estimate, &phase))
            return error_ = ERROR_SENSORLESS_ESTIMATOR_FAILED, false;
        if (!controller_.update(pos_estimate, vel_estimate, &current_setpoint))
            return error_ = ERROR_CONTROLLER_FAILED, false;
        if (!motor_.update(current_setpoint, phase))
            return error_ = ERROR_MOTOR_FAILED, false;
        return true;
    });
    set_step_dir_enabled(false);
    return error_ == ERROR_NO_ERROR;
}

bool Axis::run_closed_loop_control_loop() {
    set_step_dir_enabled(config_.enable_step_dir);
    run_control_loop([this](){
        float pos_estimate, vel_estimate, phase, current_setpoint;

        // We update the sensorless estimator just in case someone needs the output for testing
        sensorless_estimator_.update(nullptr, nullptr, nullptr);
        if (!encoder_.update(&pos_estimate, &vel_estimate, &phase))
            return error_ = ERROR_ENCODER_FAILED, false;
        if (!controller_.update(pos_estimate, vel_estimate, &current_setpoint))
            return error_ = ERROR_CONTROLLER_FAILED, false;
        if (!motor_.update(current_setpoint, phase))
            return error_ = ERROR_MOTOR_FAILED, false;
        return true;
    });
    set_step_dir_enabled(false);
    return error_ == ERROR_NO_ERROR;
}

bool Axis::run_idle_loop() {
    // run_control_loop ignores missed modulation timing updates
    // if and only if we're in AXIS_STATE_IDLE
    run_control_loop([this](){
        sensorless_estimator_.update(nullptr, nullptr, nullptr);
        encoder_.update(nullptr, nullptr, nullptr);
        return true;
    });
    return error_ == ERROR_NO_ERROR;
}

// Infinite loop that does calibration and enters main control loop as appropriate
void Axis::run_state_machine_loop() {

    // Allocate the map for anti-cogging algorithm and initialize all values to 0.0f
    // TODO: Move this somewhere else
    // TODO: respect changes of CPR
    int encoder_cpr = encoder_.config_.cpr;
    controller_.anticogging_.cogging_map = (float*)malloc(encoder_cpr * sizeof(float));
    if (controller_.anticogging_.cogging_map != NULL) {
        for (int i = 0; i < encoder_cpr; i++) {
            controller_.anticogging_.cogging_map[i] = 0.0f;
        }
    }

    // arm!
    motor_.arm();
    
    for (;;) {
        // Load the task chain if a specific request is pending
        if (requested_state_ != AXIS_STATE_UNDEFINED) {
            size_t pos = 0;
            if (requested_state_ == AXIS_STATE_STARTUP_SEQUENCE) {
                if (config_.startup_motor_calibration)
                    task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                if (config_.startup_encoder_calibration)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_CALIBRATION;
                if (config_.startup_closed_loop_control)
                    task_chain_[pos++] = AXIS_STATE_CLOSED_LOOP_CONTROL;
                else if (config_.startup_sensorless_control)
                    task_chain_[pos++] = AXIS_STATE_SENSORLESS_CONTROL;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ == AXIS_STATE_FULL_CALIBRATION_SEQUENCE) {
                task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                task_chain_[pos++] = AXIS_STATE_ENCODER_CALIBRATION;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ != AXIS_STATE_UNDEFINED) {
                task_chain_[pos++] = requested_state_;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            }
            task_chain_[pos++] = AXIS_STATE_UNDEFINED;
            // TODO: bounds checking
            requested_state_ = AXIS_STATE_UNDEFINED;
        }

        // Note that current_state is a reference to task_chain_[0]

        // Validate the state before running it
        if (current_state_ > AXIS_STATE_MOTOR_CALIBRATION && !motor_.is_calibrated_)
            current_state_ = AXIS_STATE_UNDEFINED;
        if (current_state_ > AXIS_STATE_ENCODER_CALIBRATION && !encoder_.is_calibrated_)
            current_state_ = AXIS_STATE_UNDEFINED;

        // Run the specified state
        // Handlers should exit if requested_state != AXIS_STATE_UNDEFINED
        bool status;
        switch (current_state_) {
        case AXIS_STATE_MOTOR_CALIBRATION:
            status = motor_.run_calibration();
            break;

        case AXIS_STATE_ENCODER_CALIBRATION:
            status = encoder_.run_calibration();
            break;

        case AXIS_STATE_SENSORLESS_CONTROL:
            status = run_sensorless_spin_up(); // TODO: restart if desired
            if (status)
                status = run_sensorless_control_loop();
            break;

        case AXIS_STATE_CLOSED_LOOP_CONTROL:
            status = run_closed_loop_control_loop();
            break;

        case AXIS_STATE_IDLE:
            run_idle_loop();
            status = motor_.arm(); // done with idling - try to arm the motor
            break;

        default:
            error_ = ERROR_INVALID_STATE;
            status = false; // this will set the state to idle
            break;
        }

        // If the state failed, go to idle, else advance task chain
        if (!status)
            current_state_ = AXIS_STATE_IDLE;
        else
            memcpy(task_chain_, task_chain_ + 1, sizeof(task_chain_) - sizeof(task_chain_[0]));
    }

    thread_id_valid_ = false;
}
