#pragma once
#include "session_scanner.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

enum class ExportFormat { RawCSV, StandardisedJSON };
enum class BatchMode { Individual, Combined, GroupByPatient, GroupByProtocol };

struct ExportOptions {
    ExportFormat format = ExportFormat::RawCSV;
    BatchMode batchMode = BatchMode::Individual;
    std::string outputDir;
};

class DataExporter {
public:
    void exportSessions(const std::vector<SessionInfo>& sessions, const ExportOptions& options);
    void cancel();

    bool isExporting() const { return exporting_.load(); }
    float progress() const { return progress_.load(); }
    std::string statusMessage() const;
    bool isDone() const { return done_.load(); }
    bool hasError() const { return hasError_.load(); }
    std::string errorMessage() const;

private:
    void exportRawCSV(const SessionInfo& session, const std::string& destDir);
    void exportJSON(const SessionInfo& session, const std::string& destDir);

    std::thread exportThread_;
    std::atomic<bool> exporting_{false};
    std::atomic<bool> cancelFlag_{false};
    std::atomic<float> progress_{0.0f};
    std::atomic<bool> done_{false};
    std::atomic<bool> hasError_{false};

    mutable std::mutex statusMutex_;
    std::string statusMsg_;
    std::string errorMsg_;
};
