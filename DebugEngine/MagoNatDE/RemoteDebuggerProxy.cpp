/*
   Copyright (c) 2013 Aldo J. Nunez

   Licensed under the Apache License, Version 2.0.
   See the LICENSE text file for details.
*/

#include "Common.h"
#include "RemoteDebuggerProxy.h"
#include "ArchData.h"
#include "Config.h"
#include "EventCallback.h"
#include "MagoRemoteCmd_i.h"
#include "MagoRemoteEvent_i.h"
#include "RegisterSet.h"
#include "RemoteEventRpc.h"
#include "RemoteProcess.h"
#include "RpcUtil.h"
#include <MagoDECommon.h>


#define AGENT_X64_VALUE         L"Remote_x64"


namespace Mago
{
    HRESULT StartAgent( const wchar_t* sessionGuidStr )
    {
        int                 ret = 0;
        BOOL                bRet = FALSE;
        STARTUPINFO         startupInfo = { 0 };
        PROCESS_INFORMATION processInfo = { 0 };
        HandlePtr           hEventPtr;
        std::wstring        cmdLine;
        wchar_t             eventName[AgentStartupEventNameLength + 1] = AGENT_STARTUP_EVENT_PREFIX;
        wchar_t             agentPath[MAX_PATH] = L"";
        int                 agentPathLen = _countof( agentPath );
        HKEY                hKey = NULL;

        startupInfo.cb = sizeof startupInfo;

        ret = OpenRootRegKey( false, hKey );
        if ( ret != ERROR_SUCCESS )
            return HRESULT_FROM_WIN32( ret );

        ret = GetRegString( hKey, AGENT_X64_VALUE, agentPath, agentPathLen );
        RegCloseKey( hKey );
        if ( ret != ERROR_SUCCESS )
            return HRESULT_FROM_WIN32( ret );

        wcscat_s( eventName, sessionGuidStr );

        hEventPtr = CreateEvent( NULL, TRUE, FALSE, eventName );
        if ( hEventPtr.IsEmpty() )
            return GetLastHr();

        cmdLine.append( L"\"" );
        cmdLine.append( agentPath );
        cmdLine.append( L"\" -exclusive " );
        cmdLine.append( sessionGuidStr );

        bRet = CreateProcess(
            agentPath,
            &cmdLine.front(),   // not empty, so we can call it
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &startupInfo,
            &processInfo );
        if ( !bRet )
            return GetLastHr();

        HANDLE handles[] = { hEventPtr, processInfo.hProcess };
        DWORD waitRet = WaitForMultipleObjects( 
            _countof( handles ), 
            handles, 
            FALSE, 
            AgentStartupTimeoutMillis );

        CloseHandle( processInfo.hProcess );
        CloseHandle( processInfo.hThread );

        if ( waitRet == WAIT_FAILED )
            return GetLastHr();
        if ( waitRet == WAIT_TIMEOUT )
            return E_TIMEOUT;
        if ( waitRet == WAIT_OBJECT_0 + 1 )
            return E_FAIL;

        return S_OK;
    }

    HRESULT StartServer( const wchar_t* sessionGuidStr )
    {
        HRESULT         hr = S_OK;
        RPC_STATUS      rpcRet = RPC_S_OK;
        bool            registered = false;
        std::wstring    endpoint( AGENT_EVENT_IF_LOCAL_ENDPOINT_PREFIX );

        endpoint.append( sessionGuidStr );

        rpcRet = RpcServerUseProtseqEp(
            AGENT_LOCAL_PROTOCOL_SEQUENCE,
            RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
            (RPC_WSTR) endpoint.c_str(),
            NULL );
        if ( rpcRet != RPC_S_OK )
        {
            hr = HRESULT_FROM_WIN32( rpcRet );
            goto Error;
        }

        rpcRet = RpcServerRegisterIf2(
            MagoRemoteEvent_v1_0_s_ifspec,
            NULL,
            NULL,
            RPC_IF_ALLOW_LOCAL_ONLY,
            RPC_C_LISTEN_MAX_CALLS_DEFAULT,
            (unsigned int) -1,
            NULL );
        if ( rpcRet != RPC_S_OK )
        {
            hr = HRESULT_FROM_WIN32( rpcRet );
            goto Error;
        }
        registered = true;

        rpcRet = RpcServerListen( 1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE );
        if ( rpcRet != RPC_S_OK )
        {
            hr = HRESULT_FROM_WIN32( rpcRet );
            goto Error;
        }

Error:
        if ( FAILED( hr ) )
        {
            if ( registered )
                RpcServerUnregisterIf( MagoRemoteEvent_v1_0_s_ifspec, NULL, FALSE );
        }
        return hr;
    }

    HRESULT StopServer()
    {
        RpcServerUnregisterIf( MagoRemoteEvent_v1_0_s_ifspec, NULL, FALSE );
        RpcMgmtStopServerListening( NULL );
        RpcMgmtWaitServerListen();
        return S_OK;
    }

    HRESULT OpenCmdInterface( RPC_BINDING_HANDLE hBinding, const GUID& sessionGuid, HCTXCMD& hContext )
    {
        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_Open( hBinding, &sessionGuid, &hContext );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT StartClient( const wchar_t* sessionGuidStr, const GUID& sessionGuid, HCTXCMD& hContext )
    {
        HRESULT             hr = S_OK;
        RPC_STATUS          rpcRet = RPC_S_OK;
        RPC_WSTR            strBinding = NULL;
        RPC_BINDING_HANDLE  hBinding = NULL;
        std::wstring        endpoint( AGENT_CMD_IF_LOCAL_ENDPOINT_PREFIX );

        endpoint.append( sessionGuidStr );

        rpcRet = RpcStringBindingCompose(
            NULL,
            AGENT_LOCAL_PROTOCOL_SEQUENCE,
            NULL,
            (RPC_WSTR) endpoint.c_str(),
            NULL,
            &strBinding );
        if ( rpcRet != RPC_S_OK )
            return HRESULT_FROM_WIN32( rpcRet );

        rpcRet = RpcBindingFromStringBinding( strBinding, &hBinding );
        RpcStringFree( &strBinding );
        if ( rpcRet != RPC_S_OK )
            return HRESULT_FROM_WIN32( rpcRet );

        // MSDN recommends letting the RPC runtime resolve the binding, so skip RpcEpResolveBinding

        hr = OpenCmdInterface( hBinding, sessionGuid, hContext );

        // Now that we've connected and gotten a context handle, 
        // we don't need the binding handle anymore.
        RpcBindingFree( &hBinding );

        if ( FAILED( hr ) )
            return hr;

        return S_OK;
    }

    HRESULT StopClient( HCTXCMD& hContext )
    {
        __try
        {
            MagoRemoteCmd_Close( &hContext );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            RpcSsDestroyClientContext( &hContext );
        }

        return S_OK;
    }


    //------------------------------------------------------------------------
    // RemoteDebuggerProxy
    //------------------------------------------------------------------------

    RemoteDebuggerProxy::RemoteDebuggerProxy()
        :   mRefCount( 0 ),
            mSessionGuid( GUID_NULL ),
            mhContext( NULL )
    {
    }

    RemoteDebuggerProxy::~RemoteDebuggerProxy()
    {
        Shutdown();
    }

    void RemoteDebuggerProxy::AddRef()
    {
        InterlockedIncrement( &mRefCount );
    }

    void RemoteDebuggerProxy::Release()
    {
        long newRef = InterlockedDecrement( &mRefCount );
        _ASSERT( newRef >= 0 );
        if ( newRef == 0 )
        {
            delete this;
        }
    }

    HRESULT RemoteDebuggerProxy::Init( EventCallback* callback )
    {
        _ASSERT( callback != NULL );
        if ( (callback == NULL) )
            return E_INVALIDARG;

        mCallback = callback;

        return S_OK;
    }

    HRESULT RemoteDebuggerProxy::Start()
    {
        if ( mSessionGuid != GUID_NULL )
            return S_OK;

        HRESULT     hr = S_OK;
        GUID        sessionGuid = { 0 };
        wchar_t     sessionGuidStr[GUID_LENGTH + 1] = L"";
        int         ret = 0;

        hr = CoCreateGuid( &sessionGuid );
        if ( FAILED( hr ) )
            return hr;

        ret = StringFromGUID2( sessionGuid, sessionGuidStr, _countof( sessionGuidStr ) );
        _ASSERT( ret > 0 );
        if ( ret == 0 )
            return E_FAIL;

        mSessionGuid = sessionGuid;

        hr = StartAgent( sessionGuidStr );
        if ( FAILED( hr ) )
            return hr;

        hr = StartServer( sessionGuidStr );
        if ( FAILED( hr ) )
            return hr;

        SetRemoteEventCallback( this );
        hr = StartClient( sessionGuidStr, sessionGuid, mhContext );
        SetRemoteEventCallback( NULL );
        if ( FAILED( hr ) )
        {
            StopServer();
            return hr;
        }

        return S_OK;
    }

    void RemoteDebuggerProxy::Shutdown()
    {
        // When you close the client interface to the remote agent (Cmd), the agent closes its 
        // client interface to the debug engine (Event). To allow that call back, stop the client 
        // before stopping the server.

        StopClient( mhContext );
        StopServer();
    }

    HCTXCMD RemoteDebuggerProxy::GetContextHandle()
    {
        return mhContext;
    }


    //----------------------------------------------------------------------------
    // IDebuggerProxy
    //----------------------------------------------------------------------------

    HRESULT RemoteDebuggerProxy::Launch( LaunchInfo* launchInfo, ICoreProcess*& process )
    {
        _ASSERT( launchInfo != NULL );
        if ( launchInfo == NULL )
            return E_INVALIDARG;

        HRESULT hr = S_OK;
        RefPtr<RemoteProcess>   coreProc;
        MagoRemote_LaunchInfo   cmdLaunchInfo = { 0 };
        MagoRemote_ProcInfo     cmdProcInfo = { 0 };
        uint32_t                envBstrSize = 0;

        if ( launchInfo->EnvBstr != NULL )
        {
            const wchar_t* start = launchInfo->EnvBstr;
            const wchar_t* p = start;
            while ( *p != L'\0' )
            {
                p = wcschr( p, L'\0' );
                p++;
            }
            envBstrSize = p - start + 1;
        }

        cmdLaunchInfo.CommandLine = launchInfo->CommandLine;
        cmdLaunchInfo.Dir = launchInfo->Dir;
        cmdLaunchInfo.Exe = launchInfo->Exe;
        cmdLaunchInfo.EnvBstr = launchInfo->EnvBstr;
        cmdLaunchInfo.EnvBstrSize = (uint16_t) envBstrSize;
        cmdLaunchInfo.Flags = 0;

        if ( launchInfo->NewConsole )
            cmdLaunchInfo.Flags |= MagoRemote_PFlags_NewConsole;
        if ( launchInfo->Suspend )
            cmdLaunchInfo.Flags |= MagoRemote_PFlags_Suspend;

        coreProc = new RemoteProcess();
        if ( coreProc.Get() == NULL )
            return E_OUTOFMEMORY;

        hr = LaunchNoException( cmdLaunchInfo, cmdProcInfo );
        if ( FAILED( hr ) )
            return hr;

        coreProc->Init( 
            cmdProcInfo.Pid,
            cmdProcInfo.ExePath,
            (CreateMethod) cmdProcInfo.CreateMethod,
            (Address) cmdProcInfo.EntryPoint,
            cmdProcInfo.MachineType,
            NULL );
        process = coreProc.Detach();

        MIDL_user_free( cmdProcInfo.ExePath );

        return S_OK;
    }

    // Can't use __try in functions that require object unwinding. So, pull the call out.
    HRESULT RemoteDebuggerProxy::LaunchNoException( 
        MagoRemote_LaunchInfo& cmdLaunchInfo, 
        MagoRemote_ProcInfo& cmdProcInfo )
    {
        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_Launch( 
                GetContextHandle(),
                &cmdLaunchInfo,
                &cmdProcInfo );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::Attach( uint32_t pid, ICoreProcess*& process )
    {
        HRESULT hr = S_OK;
        RefPtr<RemoteProcess>   coreProc;
        MagoRemote_ProcInfo     cmdProcInfo = { 0 };

        coreProc = new RemoteProcess();
        if ( coreProc.Get() == NULL )
            return E_OUTOFMEMORY;

        hr = AttachNoException( pid, cmdProcInfo );
        if ( FAILED( hr ) )
            return hr;

        coreProc->Init( 
            cmdProcInfo.Pid,
            cmdProcInfo.ExePath,
            (CreateMethod) cmdProcInfo.CreateMethod,
            (Address) cmdProcInfo.EntryPoint,
            cmdProcInfo.MachineType,
            // TODO: ArchData
            NULL );
        process = coreProc.Detach();

        MIDL_user_free( cmdProcInfo.ExePath );

        return S_OK;
    }

    // Can't use __try in functions that require object unwinding. So, pull the call out.
    HRESULT RemoteDebuggerProxy::AttachNoException( 
        uint32_t pid, 
        MagoRemote_ProcInfo& cmdProcInfo )
    {
        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_Attach( 
                GetContextHandle(),
                pid,
                &cmdProcInfo );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::Terminate( ICoreProcess* process )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_Terminate( 
                GetContextHandle(),
                process->GetPid() );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::Detach( ICoreProcess* process )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_Detach( 
                GetContextHandle(),
                process->GetPid() );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::ResumeLaunchedProcess( ICoreProcess* process )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_ResumeProcess( 
                GetContextHandle(),
                process->GetPid() );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::ReadMemory( 
        ICoreProcess* process, 
        Address address,
        uint32_t length, 
        uint32_t& lengthRead, 
        uint32_t& lengthUnreadable, 
        uint8_t* buffer )
    {
        _ASSERT( process != NULL );
        if ( process == NULL || buffer == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_ReadMemory( 
                GetContextHandle(),
                process->GetPid(),
                address,
                length,
                &lengthRead,
                &lengthUnreadable,
                buffer );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::WriteMemory( 
        ICoreProcess* process, 
        Address address,
        uint32_t length, 
        uint32_t& lengthWritten, 
        uint8_t* buffer )
    {
        _ASSERT( process != NULL );
        if ( process == NULL || buffer == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_WriteMemory( 
                GetContextHandle(),
                process->GetPid(),
                address,
                length,
                &lengthWritten,
                buffer );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::SetBreakpoint( ICoreProcess* process, Address address )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_SetBreakpoint( 
                GetContextHandle(),
                process->GetPid(),
                address );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::RemoveBreakpoint( ICoreProcess* process, Address address )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_RemoveBreakpoint( 
                GetContextHandle(),
                process->GetPid(),
                address );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::StepOut( ICoreProcess* process, Address targetAddr, bool handleException )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_StepOut( 
                GetContextHandle(),
                process->GetPid(),
                targetAddr,
                handleException );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::StepInstruction( ICoreProcess* process, bool stepIn, bool handleException )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_StepInstruction( 
                GetContextHandle(),
                process->GetPid(),
                stepIn,
                handleException );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::StepRange( 
        ICoreProcess* process, bool stepIn, AddressRange range, bool handleException )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            MagoRemote_AddressRange cmdRange = { range.Begin, range.End };

            hr = MagoRemoteCmd_StepRange( 
                GetContextHandle(),
                process->GetPid(),
                stepIn,
                cmdRange,
                handleException );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::Continue( ICoreProcess* process, bool handleException )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_Continue( 
                GetContextHandle(),
                process->GetPid(),
                handleException );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::Execute( ICoreProcess* process, bool handleException )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_Execute( 
                GetContextHandle(),
                process->GetPid(),
                handleException );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::AsyncBreak( ICoreProcess* process )
    {
        _ASSERT( process != NULL );
        if ( process == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT hr = S_OK;

        __try
        {
            hr = MagoRemoteCmd_AsyncBreak( 
                GetContextHandle(), 
                process->GetPid() );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }

    HRESULT RemoteDebuggerProxy::GetThreadContext( 
        ICoreProcess* process, ICoreThread* thread, IRegisterSet*& regSet )
    {
        _ASSERT( process != NULL );
        _ASSERT( thread != NULL );
        if ( process == NULL || thread == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote
            || thread->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        // TODO: get flags and context size from archData

        HRESULT hr = S_OK;
        CONTEXT context = { 0 };
        DWORD contextFlags = CONTEXT_FULL 
            | CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS;

        __try
        {
            uint32_t    sizeRead = 0;

            hr = MagoRemoteCmd_GetThreadContext(
                GetContextHandle(),
                process->GetPid(),
                thread->GetTid(),
                contextFlags,
                0,
                sizeof context,
                &sizeRead,
                (byte*) &context );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        if ( FAILED( hr ) )
            return hr;

        ArchData* archData = process->GetArchData();

        hr = archData->BuildRegisterSet( &context, sizeof context, regSet );
        if ( FAILED( hr ) )
            return hr;

        return S_OK;
    }

    HRESULT RemoteDebuggerProxy::SetThreadContext( 
        ICoreProcess* process, ICoreThread* thread, IRegisterSet* regSet )
    {
        _ASSERT( process != NULL );
        _ASSERT( thread != NULL );
        _ASSERT( regSet != NULL );
        if ( process == NULL || thread == NULL || regSet == NULL )
            return E_INVALIDARG;

        if ( process->GetProcessType() != CoreProcess_Remote
            || thread->GetProcessType() != CoreProcess_Remote )
            return E_INVALIDARG;

        HRESULT         hr = S_OK;
        const void*     contextBuf = NULL;
        uint32_t        contextSize = 0;

        if ( !regSet->GetThreadContext( contextBuf, contextSize ) )
            return E_FAIL;

        __try
        {
            hr = MagoRemoteCmd_SetThreadContext(
                GetContextHandle(),
                process->GetPid(),
                thread->GetTid(),
                contextSize,
                (byte*) contextBuf );
        }
        __except ( CommonRpcExceptionFilter( RpcExceptionCode() ) )
        {
            hr = HRESULT_FROM_WIN32( RpcExceptionCode() );
        }

        return hr;
    }


    const GUID& RemoteDebuggerProxy::GetSessionGuid()
    {
        return mSessionGuid;
    }

    void RemoteDebuggerProxy::OnProcessStart( uint32_t pid )
    {
        mCallback->OnProcessStart( pid );
    }

    void RemoteDebuggerProxy::OnProcessExit( uint32_t pid, DWORD exitCode )
    {
        mCallback->OnProcessExit( pid, exitCode );
    }

    void RemoteDebuggerProxy::OnThreadStart( uint32_t pid, MagoRemote_ThreadInfo* threadInfo )
    {
        if ( threadInfo == NULL )
            return;

        RefPtr<RemoteThread> coreThread;

        coreThread = new RemoteThread( 
            threadInfo->Tid, 
            (Address) threadInfo->StartAddr, 
            (Address) threadInfo->TebBase );
        if ( coreThread.Get() == NULL )
            return;

        mCallback->OnThreadStart( pid, coreThread );
    }

    void RemoteDebuggerProxy::OnThreadExit( uint32_t pid, DWORD threadId, DWORD exitCode )
    {
        mCallback->OnThreadExit( pid, threadId, exitCode );
    }

    void RemoteDebuggerProxy::OnModuleLoad( uint32_t pid, MagoRemote_ModuleInfo* modInfo )
    {
        if ( modInfo == NULL )
            return;

        RefPtr<RemoteModule> coreModule;

        coreModule = new RemoteModule( 
            (Address) modInfo->ImageBase, 
            (Address) modInfo->PreferredImageBase,
            modInfo->Size,
            modInfo->MachineType,
            modInfo->Path );
        if ( coreModule.Get() == NULL )
            return;

        mCallback->OnModuleLoad( pid, coreModule );
    }

    void RemoteDebuggerProxy::OnModuleUnload( uint32_t pid, MagoRemote_Address baseAddr )
    {
        mCallback->OnModuleUnload( pid, (Address) baseAddr );
    }

    void RemoteDebuggerProxy::OnOutputString( uint32_t pid, const wchar_t* outputString )
    {
        if ( outputString == NULL )
            return;

        mCallback->OnOutputString( pid, outputString );
    }

    void RemoteDebuggerProxy::OnLoadComplete( uint32_t pid, DWORD threadId )
    {
        mCallback->OnLoadComplete( pid, threadId );
    }

    MagoRemote_RunMode RemoteDebuggerProxy::OnException( 
        uint32_t pid, 
        DWORD threadId, 
        bool firstChance, 
        unsigned int recordCount,
        MagoRemote_ExceptionRecord* exceptRecords )
    {
        if ( exceptRecords == NULL )
            return MagoRemote_RunMode_Run;

        EXCEPTION_RECORD    exceptRec = { 0 };

        // TODO: more than 1 record
        exceptRec.ExceptionAddress = (void*) exceptRecords[0].ExceptionAddress;
        exceptRec.ExceptionCode = exceptRecords[0].ExceptionCode;
        exceptRec.ExceptionFlags = exceptRecords[0].ExceptionFlags;
        exceptRec.ExceptionRecord = NULL;
        exceptRec.NumberParameters = exceptRecords[0].NumberParameters;

        for ( DWORD j = 0; j < exceptRec.NumberParameters; j++ )
        {
            exceptRec.ExceptionInformation[j] = (ULONG_PTR) exceptRecords[0].ExceptionInformation[j];
        }

        return (MagoRemote_RunMode) 
            mCallback->OnException( pid, threadId, firstChance, &exceptRec );
    }

    MagoRemote_RunMode RemoteDebuggerProxy::OnBreakpoint( 
        uint32_t pid, uint32_t threadId, MagoRemote_Address address, bool embedded )
    {
        return (MagoRemote_RunMode) 
            mCallback->OnBreakpoint( pid, threadId, (Address) address, embedded );
    }

    void RemoteDebuggerProxy::OnStepComplete( uint32_t pid, uint32_t threadId )
    {
        mCallback->OnStepComplete( pid, threadId );
    }

    void RemoteDebuggerProxy::OnAsyncBreakComplete( uint32_t pid, uint32_t threadId )
    {
        mCallback->OnAsyncBreakComplete( pid, threadId );
    }

    MagoRemote_ProbeRunMode RemoteDebuggerProxy::OnCallProbe( 
        uint32_t pid, 
        uint32_t threadId, 
        MagoRemote_Address address, 
        MagoRemote_AddressRange* thunkRange )
    {
        if ( thunkRange == NULL )
            return MagoRemote_PRunMode_Run;

        AddressRange    execThunkRange = { 0 };

        ProbeRunMode mode = mCallback->OnCallProbe(
            pid,
            threadId, 
            (Address) address,
            execThunkRange );

        thunkRange->Begin = execThunkRange.Begin;
        thunkRange->End = execThunkRange.End;

        return (MagoRemote_ProbeRunMode) mode;
    }
}
