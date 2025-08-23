

#include <random>

#include "lc_configs.h"
#include "lc_trace_id_generator.h"

void lc::fs::__generate_trace_type_name(TraceTypeID      type_id,
                                           std::stringstream &trace_name) {
    switch (type_id) {
        case TraceTypeID::WriteTask : trace_name << "write_task"; break;
        case TraceTypeID::FlushTask : trace_name << "flush_task"; break;
        case TraceTypeID::BackgroundFlushTask :
            trace_name << "background_flush_task";
            break;
        case TraceTypeID::ReadTask  : trace_name << "read_task"; break;
        case TraceTypeID::BlockTask : trace_name << "block_task"; break;
        case TraceTypeID::InodeTask : trace_name << "inode_task"; break;
        case TraceTypeID::DirectoryTask :
            trace_name << "directory_task";
            break;
        default : trace_name << "unknown_task";
    }
}

void lc::fs::__uuid_v4_generate(std::stringstream &track_name) {
    std::random_device              rd;
    std::mt19937                    gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    auto to_hex = [](int value) -> char { return "0123456789abcdef"[value]; };

    for (int i = 0; i < 8; ++i) {
        track_name << to_hex(dis(gen));
    }
    track_name << '-';

    for (int i = 0; i < 4; ++i) {
        track_name << to_hex(dis(gen));
    }
    track_name << '-';
    track_name << '4';  // Version 4 UUID

    for (int i = 0; i < 3; ++i) {
        track_name << to_hex(dis(gen));
    }
    track_name << '-';

    track_name << to_hex(8 + dis(gen) % 4);
    for (int i = 0; i < 3; ++i) {
        track_name << to_hex(dis(gen));
    }
    track_name << '-';

    for (int i = 0; i < 12; ++i) {
        track_name << to_hex(dis(gen));
    }
}

void lc::fs::generate_trace_id(TraceTypeID type_id,
                                  std::string  &trace_id) {
    LC_ASSERT(trace_id.empty(), "trace_id must be empty before generation");
    std::stringstream trace_name;
    __generate_trace_type_name(type_id, trace_name);
    trace_name << "-";
    __uuid_v4_generate(trace_name);
    trace_id = trace_name.str();
}
