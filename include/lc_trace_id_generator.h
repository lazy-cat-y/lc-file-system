#ifndef LC_TRACE_ID_GENERATOR_H
#define LC_TRACE_ID_GENERATOR_H

// <task type name> + "-" + "uuid"

#include <sstream>
#include <string>

#include "lc_configs.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

enum class TraceTypeID {
    WriteTask,
    FlushTask,
    BackgroundFlushTask,
    ReadTask,
    BlockTask,
    InodeTask,
    DirectoryTask,
    // For future use
    UnknownTask,
};

void __generate_trace_type_name(TraceTypeID      type_id,
                                   std::stringstream &trace_name);

void __uuid_v4_generate(std::stringstream &uuid);

void generate_trace_id(TraceTypeID type_id, std::string &trace_id);

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_TRACE_ID_GENERATOR_H
