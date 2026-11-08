#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

/** @brief Order sent to an actuator. */
struct Command {
    int identifier;
    constexpr explicit Command(int commandIdentifier) : identifier(commandIdentifier) {}
};

class CommandHandler {
public:
    virtual void handle(Command command) = 0;
    virtual ~CommandHandler() = default;
};

#endif // COMMAND_HANDLER_H
