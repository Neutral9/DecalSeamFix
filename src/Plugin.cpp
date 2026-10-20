#include "DecalHook.h"

namespace
{
    void SetupLog()
    {
        auto path = logger::log_directory();
        if (!path) {
            return;
        }
        *path /= "DecalSeamFix.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        log->set_pattern("[%H:%M:%S.%e] %v");
        spdlog::set_default_logger(std::move(log));
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SetupLog();
    DecalHook::Install();
    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) { DecalHook::OnMessage(a_msg->type); });
    return true;
}
