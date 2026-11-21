// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/core.h"
#include "core/hle/result.h"
#include "core/hle/service/wlan/wlan.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"
#include "core/hle/service/server_manager.h"

namespace Service::WLAN {

class ILocalManager final : public ServiceFramework<ILocalManager> {
public:
    explicit ILocalManager(Core::System& system_)
        : ServiceFramework{system_, "wlan:lcl"}
    {
        static const FunctionInfo functions[] = {
            { 0, nullptr, "OpenMasterMode" },
            { 1, nullptr, "CloseMasterMode" },
            { 2, nullptr, "OpenClientMode" },
            { 3, nullptr, "CloseClientMode" },
            { 4, nullptr, "OpenSpectatorMode" },
            { 5, nullptr, "CloseSpectatorMode" },
            { 6, nullptr, "GetMacAddress_2" },
            { 7, nullptr, "CreateBss" },
            { 8, nullptr, "DestroyBss" },
            { 9, nullptr, "StartScan_2" },
            { 10, nullptr, "StopScan_2" },
            { 11, nullptr, "Connect_2" },
            { 12, nullptr, "CancelConnect_2" },
            { 13, nullptr, "Join" },
            { 14, nullptr, "CancelJoin" },
            { 15, nullptr, "Disconnect_2" },
            { 16, nullptr, "SetBeaconLostCount" },
            { 17, nullptr, "GetSystemEvent_2" },
            { 18, nullptr, "GetConnectionStatus_2" },
            { 19, nullptr, "GetClientStatus" },
            { 20, nullptr, "GetBssIndicationEvent" },
            { 21, nullptr, "GetBssIndicationInfo" },
            { 22, nullptr, "GetState_2" },
            { 23, nullptr, "GetAllowedChannels" },
            { 24, nullptr, "AddIe" },
            { 25, nullptr, "DeleteIe" },
            { 26, nullptr, "PutFrameRaw" },
            { 27, nullptr, "CancelGetFrame" },
            { 28, nullptr, "CreateRxEntry" },
            { 29, nullptr, "DeleteRxEntry" },
            { 30, nullptr, "AddEthertypeToRxEntry" },
            { 31, nullptr, "DeleteEthertypeFromRxEntry" },
            { 32, nullptr, "AddMatchingDataToRxEntry" },
            { 33, nullptr, "RemoveMatchingDataFromRxEntry" },
            { 34, nullptr, "GetScanResult_2" },
            { 35, nullptr, "PutActionFrameOneShot" },
            { 36, nullptr, "SetActionFrameWithBeacon" },
            { 37, nullptr, "CancelActionFrameWithBeacon" },
            { 38, nullptr, "CreateRxEntryForActionFrame" },
            { 39, nullptr, "DeleteRxEntryForActionFrame" },
            { 40, nullptr, "AddSubtypeToRxEntryForActionFrame" },
            { 41, nullptr, "DeleteSubtypeFromRxEntryForActionFrame" },
            { 42, nullptr, "CancelGetActionFrame" },
            { 43, nullptr, "GetRssi_2" },
            { 44, nullptr, "SetMaxAssociationNumber" },
            { 45, nullptr, "OpenLcsMasterMode" },
            { 46, nullptr, "CloseLcsMasterMode" },
            { 47, nullptr, "OpenLcsClientMode" },
            { 48, nullptr, "CloseLcsClientMode" },
            { 49, nullptr, "GetChannelStats" },
            { 50, nullptr, "Cmd50" },
            { 51, nullptr, "Cmd51" },
            { 52, nullptr, "Cmd52" },
        };
        RegisterHandlers(functions);
    }
};

class ILocalGetFrame final : public ServiceFramework<ILocalGetFrame> {
public:
    explicit ILocalGetFrame(Core::System& system_)
        : ServiceFramework{system_, "wlan:lg"}
    {
        static const FunctionInfo functions[] = {
            { 0, nullptr, "GetFrameRaw" },
        };
        RegisterHandlers(functions);
    }
};

class ILocalGetActionFrame final : public ServiceFramework<ILocalGetActionFrame> {
public:
    explicit ILocalGetActionFrame(Core::System& system_)
        : ServiceFramework{system_, "wlan:lga"}
    {
        static const FunctionInfo functions[] = {
            { 0, nullptr, "GetActionFrame" },
        };
        RegisterHandlers(functions);
    }
};

class ISocketGetFrame final : public ServiceFramework<ISocketGetFrame> {
public:
    explicit ISocketGetFrame(Core::System& system_)
        : ServiceFramework{system_, "wlan:sg"}
    {
        static const FunctionInfo functions[] = {
            { 0, nullptr, "GetFrameRaw" },
        };
        RegisterHandlers(functions);
    }
};

class ISocketManager final : public ServiceFramework<ISocketManager> {
public:
    explicit ISocketManager(Core::System& system_)
        : ServiceFramework{system_, "wlan:soc"}
    {
        static const FunctionInfo functions[] = {
            { 0, nullptr, "PutFrameRaw_2" },
            { 1, nullptr, "CancelGetFrame_2" },
            { 2, nullptr, "CreateRxEntry_2" },
            { 3, nullptr, "DeleteRxEntry_2" },
            { 4, nullptr, "AddEthertypeToRxEntry_2" },
            { 5, nullptr, "DeleteEthertypeFromRxEntry_2" },
            { 6, nullptr, "GetMacAddress_3" },
            { 7, nullptr, "SwitchTsfTimerFunction" },
            { 8, nullptr, "GetDeltaTimeBetweenSystemAndTsf" },
            { 9, nullptr, "RegisterSharedMemory" },
            { 10, nullptr, "UnregisterSharedMemory" },
            { 11, nullptr, "EnableSharedMemory" },
            { 12, nullptr, "SetMulticastFilter" },
        };
        RegisterHandlers(functions);
    }
};

class IDetectManager final : public ServiceFramework<IDetectManager> {
public:
    explicit IDetectManager(Core::System& system_)
        : ServiceFramework{system_, "wlan:dtc"}
    {
        static const FunctionInfo functions[] = {
            { 0, nullptr, "Cmd0" },
            { 1, nullptr, "Cmd1" },
            { 2, nullptr, "Cmd2" },
            { 3, nullptr, "Cmd3" },
            { 4, nullptr, "Cmd4" },
            { 5, nullptr, "Cmd5" },
            { 6, nullptr, "Cmd6" },
            { 7, nullptr, "Cmd7" },
            { 8, nullptr, "Cmd8" },
            { 9, nullptr, "Cmd9" },
            { 10, nullptr, "Cmd10" },
            { 11, nullptr, "Cmd11" },
            { 12, nullptr, "Cmd12" },
            { 13, nullptr, "Cmd13" },
            { 14, nullptr, "Cmd14" },
            { 15, nullptr, "Cmd15" },
            { 16, nullptr, "Cmd16" },
            { 17, nullptr, "Cmd17" },
            { 18, nullptr, "Cmd18" },
            { 19, nullptr, "Cmd19" },
            { 20, nullptr, "Cmd20" },
            { 21, nullptr, "Cmd21" },
            { 22, nullptr, "Cmd22" },
            { 23, nullptr, "Cmd23" },
            { 24, nullptr, "Cmd24" },
            { 25, nullptr, "Cmd25" },
            { 26, nullptr, "Cmd26" },
            { 27, nullptr, "Cmd27" },
        };
        RegisterHandlers(functions);
    }
};

class IPrivateServiceCreator final : public ServiceFramework<IPrivateServiceCreator> {
public:
    explicit IPrivateServiceCreator(Core::System& system_)
        : ServiceFramework{system_, "wlan:p"}
    {
        static const FunctionInfo functions[] = {
            { 0, nullptr, "CreateWirelessCommunicationService" },
            { 1, nullptr, "CreatePrivateWirelessCommunicationService" },
        };
        RegisterHandlers(functions);
    }
};

class ISfDriverServiceCreator final : public ServiceFramework<ISfDriverServiceCreator> {
public:
    explicit ISfDriverServiceCreator(Core::System& system_)
        : ServiceFramework{system_, "wlan:nd"}
    {
        static const FunctionInfo functions[] = {
            { 0, nullptr, "CreateDriverService" },
        };
        RegisterHandlers(functions);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    server_manager->RegisterNamedService("wlan:lcl", std::make_shared<ILocalManager>(system), 10);
    server_manager->RegisterNamedService("wlan:lg", std::make_shared<ILocalGetFrame>(system), 10);
    server_manager->RegisterNamedService("wlan:lga", std::make_shared<ILocalGetActionFrame>(system), 10);
    server_manager->RegisterNamedService("wlan:sg", std::make_shared<ISocketGetFrame>(system), 10);
    server_manager->RegisterNamedService("wlan:soc", std::make_shared<ISocketManager>(system), 10);
    server_manager->RegisterNamedService("wlan:dtc", std::make_shared<IDetectManager>(system), 4);
    server_manager->RegisterNamedService("wlan:p", std::make_shared<IPrivateServiceCreator>(system), 30);
    server_manager->RegisterNamedService("wlan:nd", std::make_shared<ISfDriverServiceCreator>(system), 5);
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::WLAN
