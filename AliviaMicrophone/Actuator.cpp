/**
* @file Actuator.cpp
 * @brief Implements the Actuator base class.
 *
 * Provides core functionality for actuators in the Modest IoT Nano-framework, including command
 * propagation to an assigned handler. Subclasses should configure hardware and define specific
 * command execution logic.
 *
 * @author Angel Velasquez
 * @date 2026-06-06
 * @version 1.0
 */

/*
 * This file is part of the Modest IoT Nano-framework (C++ Edition).
 * Copyright (c) 2025-2026 Angel Velasquez
 *
 * Licensed under the MIT License.
 * See LICENSE.md in the root of this repository for full terms.
 */

#include "Actuator.h"

/**
 * @brief Stores the target GPIO pin and optional command handler for this actuator.
 * @param signalGpioPin GPIO pin associated with the actuator.
 * @param parentHandler Optional parent handler used for command propagation.
 */
Actuator::Actuator(int signalGpioPin, CommandHandler* parentHandler)
    : pin(signalGpioPin), commandHandler(parentHandler) {}

/**
 * @brief Executes the command locally and then forwards it to the configured handler.
 * @param command Command to handle.
 */
void Actuator::handle(Command command) {
    // 1. Process concrete hardware output actions
    executeCommand(command);

    // 2. Seamlessly propagate monitoring metrics upstream
    if (commandHandler != nullptr) {
        commandHandler->handle(command);
    }}

/**
 * @brief Replaces the current propagation target for this actuator.
 * @param parentHandler Pointer to the new command handler.
 */
void Actuator::setHandler(CommandHandler* parentHandler) {
    commandHandler = parentHandler;
}