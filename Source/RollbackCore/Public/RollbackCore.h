// Copyright (c) 2026 GregOrigin. All Rights Reserved.

#pragma once
#include "Modules/ModuleManager.h"

ROLLBACKCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogRollbackCore, Log, All);

class FRollbackCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};