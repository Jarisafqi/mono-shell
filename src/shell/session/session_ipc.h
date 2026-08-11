#pragma once

class ConfigService;
class IpcService;
class SessionActionRunner;

void registerSessionIpc(IpcService& ipc, SessionActionRunner& runner, ConfigService& config);
