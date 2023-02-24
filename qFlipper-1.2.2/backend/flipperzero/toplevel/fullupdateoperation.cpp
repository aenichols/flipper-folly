#include "fullupdateoperation.h"

#include <QDebug>
#include <QDirIterator>
#include <QLoggingCategory>

#include "flipperzero/devicestate.h"
#include "flipperzero/utilityinterface.h"
#include "flipperzero/utility/updateprepareoperation.h"
#include "flipperzero/utility/directoryuploadoperation.h"
#include "flipperzero/utility/startupdateroperation.h"
#include "flipperzero/utility/storageinforefreshoperation.h"
#include "flipperzero/utility/regionprovisioningoperation.h"

#include "tarzipuncompressor.h"
#include "tempdirectories.h"
#include "remotefilefetcher.h"

#define REMOTE_DIR "/ext/update"

Q_DECLARE_LOGGING_CATEGORY(CATEGORY_DEBUG)

static inline const QString getBaseName(const QString &url)
{
    const auto start = url.lastIndexOf('/') + 1;
    const auto end = url.lastIndexOf('.');
    return url.mid(start, end - start);
}

using namespace Flipper;
using namespace Zero;

FullUpdateOperation::FullUpdateOperation(UtilityInterface *utility, DeviceState *state, const Updates::VersionInfo &versionInfo, QObject *parent):
    AbstractTopLevelOperation(state, parent),
    m_updateFile(nullptr),
    m_utility(utility),
    m_versionInfo(versionInfo)
{}

FullUpdateOperation::FullUpdateOperation(UtilityInterface *utility, DeviceState *deviceState, const QUrl &bundleUrl, QObject *parent):
    AbstractTopLevelOperation(deviceState, parent),
    m_updateFile(new QFile(bundleUrl.toLocalFile(), this)),
    m_utility(utility)
{}

FullUpdateOperation::~FullUpdateOperation()
{
    deviceState()->setAllowVirtualDisplay(true);
}

const QString FullUpdateOperation::description() const
{
    return QStringLiteral("Full Update @%1").arg(deviceState()->name());
}

void FullUpdateOperation::nextStateLogic()
{
    if(operationState() == Ready) {
        setOperationState(ProvisioninigRegion);
        provisionRegionData();

    } else if(operationState() == ProvisioninigRegion) {
        setOperationState(CheckingStorage);
        checkStorage();

    } else if(operationState() == CheckingStorage) {
        if(!m_updateFile) {
            setOperationState(FetchingUpdate);
            fetchUpdateFile();
        } else {
            setOperationState(PreparingLocalUpdate);
            prepareLocalUpdate();
        }

    } else if(operationState() == FetchingUpdate ||
              operationState() == PreparingLocalUpdate) {
        setOperationState(ExtractingUpdate);
        extractUpdate();

    } else if(operationState() == ExtractingUpdate) {
        setOperationState(PreparingUpdateDir);
        prepareUpdateDir();

    } else if(operationState() == PreparingUpdateDir) {
        setOperationState(UploadingUpdateDir);
        uploadUpdateDir();

    } else if(operationState() == UploadingUpdateDir) {
        setOperationState(WaitingForUpdate);
        startUpdate();

    } else if(operationState() == WaitingForUpdate) {
        finish();
    }
}

void FullUpdateOperation::provisionRegionData()
{
    auto *operation = m_utility->provisionRegionData();
    connect(operation, &AbstractOperation::finished, this, [=]() {
        if(operation->isError()) {
            qCInfo(CATEGORY_DEBUG) << "Warning: failed to perform region data provisioning:" << operation->errorString();
        }

        advanceOperationState();
    });
}

void FullUpdateOperation::checkStorage()
{
    deviceState()->setStatusString(QStringLiteral("Checking storage..."));

    auto *operation = m_utility->refreshStorageInfo();
    connect(operation, &AbstractOperation::finished, this, [=]() {
        if(operation->isError()) {
            finishWithError(BackendError::OperationError, QStringLiteral("Failed to check device storage"));
        } else if(!deviceState()->deviceInfo().storage.isExternalPresent) {
            finishWithError(BackendError::UnknownError, "SD Card is not installed or malfunctioning");
        } else {
            advanceOperationState();
        }
    });
}

void FullUpdateOperation::fetchUpdateFile()
{
    deviceState()->setStatusString(QStringLiteral("Fetching firmware update..."));

    const auto target = deviceState()->deviceInfo().hardware.target;
    const auto fileInfo = m_versionInfo.fileInfo(QStringLiteral("update_tgz"), target);

    if(fileInfo.target() != target) {
        finishWithError(BackendError::DataError, QStringLiteral("Required file type or target not found"));
        return;
    }

    m_updateFile = globalTempDirs->createTempFile(this);
    m_updateDirectory = globalTempDirs->subdir(getBaseName(fileInfo.url()));

    auto *fetcher = new RemoteFileFetcher(this);
    if(!fetcher->fetch(fileInfo, m_updateFile)) {
        finishWithError(fetcher->error(), fetcher->errorString());
        return;
    }

    connect(fetcher, &RemoteFileFetcher::progressChanged, this, [=](double progress) {
        deviceState()->setProgress(progress);
    });

    connect(fetcher, &RemoteFileFetcher::finished, this, [=]() {
        if(fetcher->isError()) {
            finishWithError(fetcher->error(), fetcher->errorString());
        } else {
            advanceOperationState();
        }

        fetcher->deleteLater();
    });
}

void FullUpdateOperation::prepareLocalUpdate()
{
    deviceState()->setStatusString(QStringLiteral("Preparing local firmware update..."));
    m_updateDirectory = globalTempDirs->subdir(getBaseName(m_updateFile->fileName()));
    advanceOperationState();
}

void FullUpdateOperation::extractUpdate()
{
    deviceState()->setStatusString(QStringLiteral("Extracting firmware update ..."));
    deviceState()->setProgress(-1.0);

    auto *uncompressor = new TarZipUncompressor(m_updateFile, m_updateDirectory, this);

    connect(uncompressor, &TarZipUncompressor::finished, this, [=]() {
        if(uncompressor->isError()) {
            finishWithError(uncompressor->error(), uncompressor->errorString());
        } else {
            advanceOperationState();
        }

        uncompressor->deleteLater();
    });
}

void FullUpdateOperation::prepareUpdateDir()
{
    deviceState()->setStatusString(QStringLiteral("Preparing firmware update ..."));
    deviceState()->setProgress(-1.0);

    if(!findAndCdToUpdateDir()) {
        finishWithError(BackendError::DataError, QStringLiteral("Cannot find update directory"));
        return;
    }

    auto *operation = m_utility->prepareUpdateDirectory(m_updateDirectory.dirName().toLocal8Bit(), QByteArrayLiteral(REMOTE_DIR));

    connect(operation, &AbstractOperation::finished, this, [=]() {
        if(operation->isError()) {
            finishWithError(operation->error(), operation->errorString());
            return;

        } else if(operation->updateDirectoryExists()) {
            qCDebug(CATEGORY_DEBUG) << "Update package has been already uploaded, skipping to update...";
            setOperationState(FullUpdateOperation::UploadingUpdateDir);
        }

        advanceOperationState();
    });
}

void FullUpdateOperation::uploadUpdateDir()
{
    deviceState()->setStatusString(QStringLiteral("Uploading firmware update ..."));

    auto *operation = m_utility->uploadDirectory(m_updateDirectory.absolutePath(), QByteArrayLiteral(REMOTE_DIR));

    connect(operation, &AbstractOperation::progressChanged, this, [=]() {
        deviceState()->setProgress(operation->progress());
    });

    connect(operation, &AbstractOperation::finished, this, [=]() {
        if(operation->isError()) {
            finishWithError(operation->error(), operation->errorString());
        } else {
            advanceOperationState();
        }
    });
}

void FullUpdateOperation::startUpdate()
{
    deviceState()->setAllowVirtualDisplay(false);

    const auto manifestPath = QStringLiteral("%1/%2/update.fuf").arg(QStringLiteral(REMOTE_DIR), m_updateDirectory.dirName());
    auto *operation = m_utility->startUpdater(manifestPath.toLocal8Bit());

    connect(operation, &AbstractOperation::finished, this, [=]() {
        if(operation->isError()) {
            finishWithError(operation->error(), operation->errorString());
        } else {
            advanceOperationState();
        }
    });
}

bool FullUpdateOperation::findAndCdToUpdateDir()
{
    m_updateDirectory.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);

    QDirIterator it(m_updateDirectory);

    while(it.hasNext()) {
        it.next();

        const auto &fileInfo = it.fileInfo();
        const auto fileName = fileInfo.fileName();

        if(fileInfo.isDir() && m_updateDirectory.dirName().endsWith(fileName)) {
            m_updateDirectory.cd(fileName);
            return true;
        }
    }

    return false;
}
