#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "dist/common/config.hpp"
#include "dist/common/env_loader.hpp"
#include "dist/common/utils.hpp"
#include "dist/common/version.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic_bool g_keep_running{true};
constexpr auto kZmqRetryBackoff = std::chrono::seconds(1);

// Make a best-effort attempt at connecting until upstream is ready.
bool connect_with_retry(zmq::socket_t& socket, const std::string& endpoint) {
    int attempt = 1;
    while (g_keep_running.load()) {
        try {
            socket.connect(endpoint);
            return true;
        } catch (const zmq::error_t& ex) {
            spdlog::warn("Failed to connect SUB socket to {} (attempt {}): {}. Waiting for feature "
                         "extractor...",
                         endpoint,
                         attempt,
                         ex.what());
            ++attempt;
            std::this_thread::sleep_for(kZmqRetryBackoff);
        }
    }
    return false;
}

// Keep filenames filesystem-friendly (avoid spaces or exotic characters).
std::string sanitize_filename(std::string value) {
    for (char& ch : value) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' ||
              ch == '.')) {
            ch = '_';
        }
    }
    return value;
}

// Idempotent table creation so the logger can start from a blank directory.
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

// Shared logic to honor CLI flags, env overrides, and repo defaults.
fs::path resolve_env_path(const std::string& cli_env_path,
                          const char* env_override,
                          const fs::path& root) {
    if (!cli_env_path.empty()) {
        return cli_env_path;
    }
    if (env_override != nullptr) {
        return env_override;
    }
    return root / ".env";
}

// Create directories declared in config so later file writes do not fail.
bool ensure_output_directories(const dist::common::DataLoggerConfig& cfg) {
    try {
        std::filesystem::create_directories(cfg.raw_image_dir);
        std::filesystem::create_directories(cfg.annotated_image_dir);
        if (!cfg.db_path.parent_path().empty()) {
            std::filesystem::create_directories(cfg.db_path.parent_path());
        }
        return true;
    } catch (const std::exception& ex) {
        spdlog::error("Failed to create storage directories: {}", ex.what());
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Provide a minimal CLI so local testing is ergonomic.
    CLI::App app{"Data Logger - consumes processed frames and stores them"};
    std::string cli_env_path;
    std::string cli_log_level;
    app.add_option("--env", cli_env_path, "Path to the .env file (overrides DIST_ENV_PATH)");
    app.add_option("--log-level", cli_log_level,
                   "Override log level (trace|debug|info|warn|error|critical)");

    CLI11_PARSE(app, argc, argv);

    // Load configuration from .env (CLI override takes precedence).
    dist::common::EnvLoader loader;
    const auto root = fs::current_path();
    const char* env_override = std::getenv("DIST_ENV_PATH");
    fs::path env_path = resolve_env_path(cli_env_path, env_override, root);
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

    if (!ensure_output_directories(config.logger)) {
        return 1;
    }

    // Database bootstrap + schema creation.
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

    // Networking stack (SUB socket facing the extractor pipeline).
    zmq::context_t context{1};
    zmq::socket_t sink{context, zmq::socket_type::sub};
    sink.set(zmq::sockopt::rcvhwm, 100);
    sink.set(zmq::sockopt::rcvtimeo, 500);
    sink.set(zmq::sockopt::linger, 0);
    sink.set(zmq::sockopt::subscribe, "");
    if (!connect_with_retry(sink, config.logger.sub_endpoint)) {
        return 1;
    }

    spdlog::info("[data_logger] Dist Imaging Services v{}", dist::common::version());
    spdlog::info("Listening for processed frames on {}", config.logger.sub_endpoint);
    spdlog::info("Saving PNGs to {}", config.logger.raw_image_dir.string());
    spdlog::info("Saving annotated PNGs to {}", config.logger.annotated_image_dir.string());
    spdlog::info("Persisting metadata to {}", config.logger.db_path.string());

    auto last_wait_log = std::chrono::steady_clock::now();

    // Main receive/insert loop.
    while (g_keep_running.load()) {
        zmq::message_t header_msg;
        zmq::message_t descriptors_msg;
        zmq::message_t image_msg;
        zmq::message_t annotated_msg;
        bool has_annotated = false;

        try {
            // Expect [header][descriptors][raw image][optional annotated image].
            if (!sink.recv(header_msg, zmq::recv_flags::none)) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_wait_log > std::chrono::seconds(5)) {
                    spdlog::info("Waiting for processed frames on {}", config.logger.sub_endpoint);
                    last_wait_log = now;
                }
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

        const int part_count = has_annotated ? 4 : 3;
        spdlog::debug("Received {} parts from extractor", part_count);

        nlohmann::json header;
        try {
            header = nlohmann::json::parse(header_msg.to_string());
        } catch (const std::exception& ex) {
            spdlog::warn("Failed to parse metadata JSON: {}", ex.what());
            continue;
        }

        // Pull frequently used metadata upfront for clarity.
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

        // Persist file names with monotonically increasing prefix.
        std::ostringstream oss;
        oss << "frame_" << std::setw(6) << std::setfill('0') << std::max(frame_id, 0) << "_"
            << sanitize_filename(processed_timestamp) << ".png";
        const fs::path image_path = config.logger.raw_image_dir / oss.str();

        // Persist raw PNG to disk so downstream inspection is trivial.
        std::ofstream out(image_path, std::ios::binary);
        if (!out.good()) {
            spdlog::error("Failed to open {} for writing", image_path.string());
            continue;
        }
        out.write(static_cast<const char*>(image_blob.data()),
                  static_cast<std::streamsize>(image_blob.size()));
        out.close();

        fs::path annotated_path;
        bool annotated_saved = false;
        if (has_annotated && annotated_msg.size() > 0) {
            // Annotated frames mirror the raw naming convention with suffix.
            std::ostringstream aoss;
            aoss << "frame_" << std::setw(6) << std::setfill('0') << std::max(frame_id, 0) << "_"
                 << sanitize_filename(processed_timestamp) << "_annotated.png";
            annotated_path = config.logger.annotated_image_dir / aoss.str();

            std::ofstream annotated_out(annotated_path, std::ios::binary);
            if (!annotated_out.good()) {
                spdlog::warn("Failed to open {} for writing annotated frame", annotated_path.string());
            } else {
                annotated_out.write(static_cast<const char*>(annotated_msg.data()),
                                    static_cast<std::streamsize>(annotated_msg.size()));
                annotated_out.close();
                annotated_saved = true;
            }
        }
        if (annotated_saved) {
            header["annotated_path"] = annotated_path.string();
        }

        // Prepare SQLite statement for reuse before binding.
        sqlite3_reset(insert_stmt);
        sqlite3_clear_bindings(insert_stmt);

        const std::string metadata_dump = header.dump();
        const std::string created_at = dist::common::now_iso8601();

        // Bind all values in positional order (matches INSERT statement).
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
