#include "startorch/utils/logger.h"

#include <string>
#include <chrono>
#include <format>
#include <iostream>
#include <utility>

namespace {
    constexpr std::string_view level_name(Logger::Level level) {
        switch (level) {
            case Logger::Level::DEBUG:   return "DEBUG";
            case Logger::Level::INFO:    return "INFO";
            case Logger::Level::WARNING: return "WARNING";
            case Logger::Level::ERROR:   return "ERROR";
        }
        return "UNKNOWN";
    }
}

Logger::Logger(std::string _file_name, const Level _default_level) :
    file_name(std::move(_file_name)),
    default_level(_default_level) {};

void Logger::log(const std::string message) const {
    log(message, default_level);
}

void Logger::log(const std::string message, const Level log_level) const {
    std::chrono::time_point now = std::chrono::system_clock::now();
    std::chrono::zoned_time local_time{std::chrono::current_zone(), now};
    std::cerr << std::format("{0} [{1}] {2}: {3}\n", local_time, level_name(log_level), file_name, message);
}