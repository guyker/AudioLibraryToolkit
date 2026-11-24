#pragma once
#include "MediaConversionTask.h"


class MediaConversionAsyncTask : public MediaConversionTask
{
public:

	MediaConversionAsyncTask() = delete;

	MediaConversionAsyncTask(const fs::path sourcePath, const fs::path targetPath, const fs::path targetTMPPath, bool overwriteTarget = true) : MediaConversionTask(sourcePath, targetPath, targetTMPPath, overwriteTarget) {}

	virtual int Run() override;
	virtual int PostRun() override;

	virtual ~MediaConversionAsyncTask() {}

private:

	std::future<int> _asyncResult;
};
