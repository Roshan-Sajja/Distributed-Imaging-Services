#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <zmq.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

#include "dist/common/config.hpp"
#include "dist/common/env_loader.hpp"
#include "dist/common/utils.hpp"
#include "dist/common/version.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic_bool g_keep_running{true};

std::string sanitize_filename(std::string value) {
    for (char& ch : value) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' ||
              ch == '.')) {
            ch = '_';
        }
    }
    return value;
}

void ensure_schema(sqlite3* db) {
    static constexpr const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS frames (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            frame_id INTEGER,
            loop_iteration INTEGER,
            source_timestamp TEXT,
            processed_timestamp TEXT,
            filename TEXT,
            width INTEGER,
            height INTEGER,
            channels INTEGER,
            encoding TEXT,
            keypoint_count INTEGER,
            descriptor_rows INTEGER,
            descriptor_cols INTEGER,
            descriptor_elem_size INTEGER,
            descriptor_type INTEGER,
            descriptors_bytes INTEGER,
            image_path TEXT,
            metadata_json TEXT,
            descriptors BLOB,
            created_at TEXT
        );
    )SQL";

    char* errmsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string message = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        throw std::runtime_error("Failed to create frames table: " + message);
    }
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Data Logger - consumes processed frames and stores them"};
    std::string cli_env_path;
    std::string cli_log_level;
    app.add_option("--env", cli_env_path, "Path to the .env file (overrides DIST_ENV_PATH)");
    app.add_option("--log-level", cli_log_level,
                   "Override log level (trace|debug|info|warn|error|critical)");

    CLI11_PARSE(app, argc, argv);

    dist::common::EnvLoader loader;
    const auto root = fs::current_path();
    const char* env_override = std::getenv("DIST_ENV_PATH");
    fs::path env_path = root / ".env";
    if (!cli_env_path.empty()) {
        env_path = cli_env_path;
    } else if (env_override != nullptr) {
        env_path = env_override;
    }
    if (!loader.load_from_file(env_path)) {
        spdlog::error("Failed to read environment file at {}", env_path.string());
        return 1;
    }

    const auto config = dist::common::load_app_config(loader, root);
    const auto resolved_level =
        cli_log_level.empty() ? config.global.log_level : cli_log_level;
    spdlog::set_level(dist::common::level_from_string(resolved_level));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    dist::common::install_signal_handlers(g_keep_running);

    try {
        fs::create_directories(config.logger.raw_image_dir);
        if (!config.logger.db_path.parent_path().empty()) {
            fs::create_directories(config.logger.db_path.parent_path());
        }
    } catch (const std::exception& ex) {
        spdlog::error("Failed to create storage directories: {}", ex.what());
        return 1;
    }

    sqlite3* raw_db = nullptr;
    if (sqlite3_open(config.logger.db_path.string().c_str(), &raw_db) != SQLITE_OK) {
        spdlog::error("Unable to open database at {}: {}",
                      config.logger.db_path.string(),
                      sqlite3_errmsg(raw_db));
        sqlite3_close(raw_db);
        return 1;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw_db, &sqlite3_close);

    try {
        ensure_schema(db.get());
    } catch (const std::exception& ex) {
        spdlog::error("{}", ex.what());
        return 1;
    }

    sqlite3_stmt* insert_stmt = nullptr;
    static constexpr const char* insert_sql = R"SQL(
        INSERT INTO frames (
            frame_id, loop_iteration, source_timestamp, processed_timestamp, filename,
            width, height, channels, encoding,
            keypoint_count, descriptor_rows, descriptor_cols, descriptor_elem_size,
            descriptor_type, descriptors_bytes, image_path, metadata_json, descriptors, created_at
        ) VALUES (
            ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
        );
    )SQL";

    if (sqlite3_prepare_v2(db.get(), insert_sql, -1, &insert_stmt, nullptr) != SQLITE_OK) {
        spdlog::error("Failed to prepare insert statement: {}", sqlite3_errmsg(db.get()));
        return 1;
    }

    zmq::context_t context{1};
    zmq::socket_t sink{context, zmq::socket_type::pull};
    sink.set(zmq::sockopt::rcvhwm, 100);
    sink.set(zmq::sockopt::rcvtimeo, 500);
    sink.set(zmq::sockopt::linger, 0);
    try {
        sink.bind(config.logger.sub_endpoint);
    } catch (const zmq::error_t& ex) {
        spdlog::error("Failed to bind PULL socket on {}: {}", config.logger.sub_endpoint, ex.what());
        return 1;
    }

    spdlog::info("[data_logger] Dist Imaging Services v{}", dist::common::version());
    spdlog::info("Listening for processed frames on {}", config.logger.sub_endpoint);
    spdlog::info("Saving PNGs to {}", config.logger.raw_image_dir.string());
    spdlog::info("Persisting metadata to {}", config.logger.db_path.string());

    while (g_keep_running.load()) {
        zmq::message_t header_msg;
        zmq::message_t descriptors_msg;
        zmq::message_t image_msg;
        zmq::message_t annotated_msg;
        bool has_annotated = false;

        try {
            if (!sink.recv(header_msg, zmq::recv_flags::none)) {
                continue;  // timeout
            }
            if (!header_msg.more()) {
                spdlog::warn("Discarding message missing descriptors part");
                continue;
            }
            if (!sink.recv(descriptors_msg, zmq::recv_flags::none)) {
                spdlog::warn("Incomplete multipart message (no descriptors)");
                continue;
            }
            if (!descriptors_msg.more()) {
                spdlog::warn("Discarding message missing image payload");
                continue;
            }
            if (!sink.recv(image_msg, zmq::recv_flags::none)) {
                spdlog::warn("Incomplete multipart message (no image)");
                continue;
            }
            has_annotated = image_msg.more();
            if (has_annotated) {
                if (!sink.recv(annotated_msg, zmq::recv_flags::none)) {
                    spdlog::warn("Incomplete multipart message (annotated frame missing)");
                    continue;
                }
            }
        } catch (const zmq::error_t& ex) {
            if (ex.num() == EAGAIN) {
                continue;  // timeout, try again
            }
            if (g_keep_running.load()) {
                spdlog::error("ZeroMQ receive error: {}", ex.what());
            }
            break;
        }

        spdlog::debug("Received 3 parts from extractor");

        nlohmann::json header;
        try {
            header = nlohmann::json::parse(header_msg.to_string());
        } catch (const std::exception& ex) {
            spdlog::warn("Failed to parse metadata JSON: {}", ex.what());
            continue;
        }

        const auto source = header.value("source", nlohmann::json::object());
        const int frame_id = source.value("frame_id", -1);
        const int loop_iteration = source.value("loop_iteration", 0);
        const std::string source_timestamp = source.value("timestamp", "");
        const std::string processed_timestamp =
            header.value("processed_timestamp", dist::common::now_iso8601());
        const std::string filename = source.value("filename", "frame.png");
        const int width = source.value("width", 0);
        const int height = source.value("height", 0);
        const int channels = source.value("channels", 0);
        const std::string encoding = source.value("encoding", "png");
        const std::size_t keypoint_count =
            header.value<std::size_t>("keypoint_count", 0);
        const int descriptor_rows = header.value("descriptor_rows", 0);
        const int descriptor_cols = header.value("descriptor_cols", 0);
        const int descriptor_elem_size = header.value("descriptor_elem_size", 0);
        const int descriptor_type = header.value("descriptor_type", 0);

        const auto& descriptor_blob = descriptors_msg;
        const auto& image_blob = image_msg;

        std::ostringstream oss;
        oss << "frame_" << std::setw(6) << std::setfill('0') << std::max(frame_id, 0) << "_"
            << sanitize_filename(processed_timestamp) << ".png";
        const fs::path image_path = config.logger.raw_image_dir / oss.str();

        std::ofstream out(image_path, std::ios::binary);
        if (!out.good()) {
            spdlog::error("Failed to open {} for writing", image_path.string());
            continue;
        }
        out.write(static_cast<const char*>(image_blob.data()),
                  static_cast<std::streamsize>(image_blob.size()));
        out.close();

        fs::path annotated_path;
        if (has_annotated && annotated_msg.size() > 0) {
            std::ostringstream aoss;
            aoss << "frame_" << std::setw(6) << std::setfill('0') << std::max(frame_id, 0) << "_"
                 << sanitize_filename(processed_timestamp) << "_annotated.png";
            annotated_path = config.logger.raw_image_dir / aoss.str();

            std::ofstream annotated_out(annotated_path, std::ios::binary);
            if (!annotated_out.good()) {
                spdlog::warn("Failed to open {} for writing annotated frame", annotated_path.string());
            } else {
                annotated_out.write(static_cast<const char*>(annotated_msg.data()),
                                    static_cast<std::streamsize>(annotated_msg.size()));
                annotated_out.close();
            }
            header["annotated_path"] = annotated_path.string();
        }

        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);

        const std::string metadata_dump = header.dump();
        const std::string created_at = dist::common::now_iso8601();

        int bind_index = 1;
        sqlite3_bind_int(insert_stmt, bind_index++, frame_id);
        sqlite3_bind_int(insert_stmt, bind_index++, loop_iteration);
        sqlite3_bind_text(insert_stmt, bind_index++, source_timestamp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, bind_index++, processed_timestamp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, bind_index++, filename.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert_stmt, bind_index++, width);
        sqlite3_bind_int(insert_stmt, bind_index++, height);
        sqlite3_bind_int(insert_stmt, bind_index++, channels);
        sqlite3_bind_text(insert_stmt, bind_index++, encoding.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_stmt, bind_index++, static_cast<sqlite3_int64>(keypoint_count));
        sqlite3_bind_int(insert_stmt, bind_index++, descriptor_rows);
        sqlite3_bind_int(insert_stmt, bind_index++, descriptor_cols);
        sqlite3_bind_int(insert_stmt, bind_index++, descriptor_elem_size);
        sqlite3_bind_int(insert_stmt, bind_index++, descriptor_type);
        sqlite3_bind_int(insert_stmt, bind_index++, static_cast<int>(descriptor_blob.size()));
        sqlite3_bind_text(insert_stmt, bind_index++, image_path.string().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, bind_index++, metadata_dump.c_str(), -1, SQLITE_TRANSIENT);

        if (descriptor_blob.size() > 0) {
            sqlite3_bind_blob(insert_stmt,
                              bind_index++,
                              descriptor_blob.data(),
                              static_cast<int>(descriptor_blob.size()),
                              SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_blob(insert_stmt, bind_index++, nullptr, 0, SQLITE_TRANSIENT);
        }

        sqlite3_bind_text(insert_stmt, bind_index++, created_at.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
            spdlog::error("Failed to insert frame {}: {}", frame_id, sqlite3_errmsg(db.get()));
            continue;
        }

        spdlog::info("Stored frame {} ({} keypoints, {} bytes)",
                     frame_id,
                     keypoint_count,
                     image_blob.size());
    }

    sqlite3_finalize(insert_stmt);
    spdlog::info("Data logger shutting down");
    return 0;
}
