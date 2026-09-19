#ifndef LOGGER_H
#define LOGGER_H

#include <string>

class Logger{
public:
    enum Level {
        DEBUG = 0,
        INFO = 1,
        WARNING = 2,
        ERROR = 3
    };

    // Also takes __FILE_NAME__ (a const char*) through std::string's conversion.
    Logger(std::string _file_name, const Level _default_level = DEBUG);

    void log(std::string message) const;

    void log(std::string message, Level log_level) const;

private:
    std::string file_name;
    Level default_level;
};

#endif