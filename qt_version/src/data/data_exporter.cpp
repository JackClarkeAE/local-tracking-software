#include "data_exporter.h"
#include "../recording/playback.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdio>

namespace fs = std::filesystem;

std::string DataExporter::statusMessage() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return statusMsg_;
}

std::string DataExporter::errorMessage() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return errorMsg_;
}

void DataExporter::cancel() {
    cancelFlag_.store(true);
    if (exportThread_.joinable()) exportThread_.join();
}

void DataExporter::exportSessions(const std::vector<SessionInfo>& sessions, const ExportOptions& options) {
    if (exporting_.load()) return;
    if (sessions.empty() || options.outputDir.empty()) return;

    exporting_.store(true);
    cancelFlag_.store(false);
    progress_.store(0.0f);
    done_.store(false);
    hasError_.store(false);

    if (exportThread_.joinable()) exportThread_.join();

    exportThread_ = std::thread([this, sessions, options]() {
        fs::create_directories(options.outputDir);

        for (int i = 0; i < (int)sessions.size(); i++) {
            if (cancelFlag_.load()) break;
            progress_.store((float)i / sessions.size());

            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                statusMsg_ = "Exporting: " + sessions[i].displayName + " (" + std::to_string(i + 1) + "/" + std::to_string(sessions.size()) + ")";
            }

            std::string destDir = options.outputDir;

            // Subdirectory for batch modes
            if (options.batchMode == BatchMode::GroupByPatient && !sessions[i].patientId.empty()) {
                destDir += "/" + sessions[i].patientId;
                fs::create_directories(destDir);
            } else if (options.batchMode == BatchMode::GroupByProtocol && !sessions[i].protocol.empty()) {
                std::string protoDir = sessions[i].protocol;
                for (auto& c : protoDir) if (c == ' ') c = '_';
                destDir += "/" + protoDir;
                fs::create_directories(destDir);
            }

            try {
                if (options.format == ExportFormat::RawCSV) {
                    exportRawCSV(sessions[i], destDir);
                } else {
                    exportJSON(sessions[i], destDir);
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(statusMutex_);
                errorMsg_ = std::string("Export error: ") + e.what();
                hasError_.store(true);
            }
        }

        progress_.store(1.0f);
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            statusMsg_ = "Export complete. " + std::to_string(sessions.size()) + " session(s).";
        }
        done_.store(true);
        exporting_.store(false);
    });
}

void DataExporter::exportRawCSV(const SessionInfo& session, const std::string& destDir) {
    // Copy all session files to destination
    auto copyFile = [&](const std::string& src) {
        if (src.empty()) return;
        fs::path srcPath(src);
        fs::path dstPath = fs::path(destDir) / srcPath.filename();
        fs::copy_file(srcPath, dstPath, fs::copy_options::overwrite_existing);
    };

    for (auto& csv : session.csvPaths) copyFile(csv);
    copyFile(session.flagsPath);
    copyFile(session.metadataPath);
    copyFile(session.biofeedbackAvatarPath);
    copyFile(session.biofeedbackAnglesPath);
}

void DataExporter::exportJSON(const SessionInfo& session, const std::string& destDir) {
    if (session.csvPaths.empty()) return;

    // Load frames from CSV
    PlaybackManager pm;
    pm.loadCSV(session.csvPaths[0]);
    if (!pm.isLoaded()) return;

    // Output path
    std::string outPath = destDir + "/" + session.prefix + ".json";
    FILE* f = fopen(outPath.c_str(), "w");
    if (!f) return;

    // Escape helper
    auto esc = [](const std::string& s) -> std::string {
        std::string out;
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else out += c;
        }
        return out;
    };

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": \"1.0\",\n");

    // Metadata
    fprintf(f, "  \"metadata\": {\n");
    fprintf(f, "    \"patient_id\": \"%s\",\n", esc(session.patientId).c_str());
    fprintf(f, "    \"operator\": \"%s\",\n", esc(session.operatorName).c_str());
    fprintf(f, "    \"notes\": \"%s\",\n", esc(session.notes).c_str());
    fprintf(f, "    \"date\": \"%s\",\n", esc(session.date).c_str());
    std::string camName = session.cameraNames.empty() ? "Unknown" : session.cameraNames[0];
    fprintf(f, "    \"camera\": \"%s\",\n", esc(camName).c_str());
    fprintf(f, "    \"protocol\": \"%s\",\n", esc(session.protocol).c_str());
    fprintf(f, "    \"duration_seconds\": %.1f,\n", pm.duration());
    fprintf(f, "    \"frame_count\": %d,\n", pm.frameCount());
    fprintf(f, "    \"joint_count\": %d,\n", JOINT_COUNT);
    fprintf(f, "    \"joint_names\": [");
    for (int j = 0; j < JOINT_COUNT; j++) {
        fprintf(f, "\"%s\"%s", jointName(j), (j < JOINT_COUNT - 1) ? ", " : "");
    }
    fprintf(f, "]\n");
    fprintf(f, "  },\n");

    // Frames
    fprintf(f, "  \"frames\": [\n");
    for (int i = 0; i < pm.frameCount(); i++) {
        if (cancelFlag_.load()) break;
        const PlaybackFrame* pf = pm.getFrame(i);
        if (!pf) continue;

        fprintf(f, "    {\"time\": %.6f, \"bodies\": [", pf->timeSeconds);
        for (int bi = 0; bi < (int)pf->bodies.size(); bi++) {
            auto& body = pf->bodies[bi];
            fprintf(f, "{\"id\": %u, \"joints\": [", body.id);
            for (int j = 0; j < JOINT_COUNT; j++) {
                fprintf(f, "[%.4f,%.4f,%.4f]%s",
                    body.joints[j].x, body.joints[j].y, body.joints[j].z,
                    (j < JOINT_COUNT - 1) ? "," : "");
            }
            fprintf(f, "], \"confidence\": [");
            for (int j = 0; j < JOINT_COUNT; j++) {
                fprintf(f, "%.2f%s", body.joints[j].confidence, (j < JOINT_COUNT - 1) ? "," : "");
            }
            fprintf(f, "]}%s", (bi < (int)pf->bodies.size() - 1) ? "," : "");
        }
        fprintf(f, "]}%s\n", (i < pm.frameCount() - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    // Flags
    fprintf(f, "  \"flags\": [");
    const auto& flags = pm.flags();
    for (int i = 0; i < (int)flags.size(); i++) {
        fprintf(f, "{\"time\": %.2f, \"label\": \"%s\"}%s",
            flags[i].timeSeconds, esc(flags[i].label).c_str(),
            (i < (int)flags.size() - 1) ? ", " : "");
    }
    fprintf(f, "]\n");

    fprintf(f, "}\n");
    fclose(f);
}
