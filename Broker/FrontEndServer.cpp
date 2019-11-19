#include "FrontEndServer.h"




/*++
Routine Description:

Creates the FrontEndServer object.


Arguments:

	None


Return Value:
	May throw an exception if the the allocation failed.

--*/
FrontEndServer::FrontEndServer()
{
	if (!CreatePipe())
	{
		RAISE_GENERIC_EXCEPTION("CreatePipe() failed");
	}
}


/*++
Routine Description:

Create the named pipe responsible for the communication with the GUI. To do, a Security 
Descriptor is created with Explicit Access set for Everyone (including remote clients),
to Read/Write the pipe.

Therefore, we must be careful about that as any user would be able to send some commands
to the broker pipe (and therefore to the kernel driver). 


Arguments:

	None


Return Value:
	Returns TRUE upon successful creation of the pipe, FALSE if any error occured.

--*/
BOOL FrontEndServer::CreatePipe() 
{
	BOOL fSuccess = FALSE;
	SID_IDENTIFIER_AUTHORITY SidAuthWorld = SECURITY_WORLD_SID_AUTHORITY;
	EXPLICIT_ACCESS ea[1] = { 0 };
	PACL pNewAcl = NULL;
	PSID pEveryoneSid = NULL;
	SECURITY_ATTRIBUTES SecurityAttributes = { 0 };
	SECURITY_DESCRIPTOR SecurityDescriptor = { 0 };

	do
	{
#ifdef _DEBUG
		xlog(LOG_DEBUG, L"Defining new SD for pipe\n");
#endif // _DEBUG
	

		//
		// For now, SD is set for Everyone to have RW access 
		//

		if (!::AllocateAndInitializeSid(
				&SidAuthWorld,
				1,
				SECURITY_WORLD_RID,
				0, 0, 0, 0, 0, 0, 0,
				&pEveryoneSid
			)
		)
		{
			PrintErrorWithFunctionName(L"AllocateAndInitializeSid");
			fSuccess = FALSE;
			break;
		}


		//
		// Populate the EA entry
		//
		ea[0].grfAccessPermissions = GENERIC_ALL;
		ea[0].grfAccessMode = SET_ACCESS;
		ea[0].grfInheritance = NO_INHERITANCE;
		ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
		ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
		ea[0].Trustee.ptstrName = (LPTSTR)pEveryoneSid;


		//
		// Apply the EA to the ACL
		//
		if (::SetEntriesInAcl(1, ea, NULL, &pNewAcl) != ERROR_SUCCESS)
		{
			PrintErrorWithFunctionName(L"SetEntriesInAcl");
			fSuccess = FALSE;
			break;
		}


		//
		// Set the SD to new ACL
		//
		if (!::InitializeSecurityDescriptor(&SecurityDescriptor, SECURITY_DESCRIPTOR_REVISION))
		{
			PrintErrorWithFunctionName(L"InitializeSecurityDescriptor");
			fSuccess = FALSE;
			break;
		}

		if (!::SetSecurityDescriptorDacl(&SecurityDescriptor, TRUE, pNewAcl, FALSE))
		{
			PrintErrorWithFunctionName(L"SetSecurityDescriptorDacl");
			fSuccess = FALSE;
			break;
		}


		//
		// Init the SA
		//
		SecurityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
		SecurityAttributes.lpSecurityDescriptor = &SecurityDescriptor;
		SecurityAttributes.bInheritHandle = FALSE;


#ifdef _DEBUG
		xlog(LOG_DEBUG, L"Creating named pipe '%s'...\n", CFB_PIPE_NAME);
#endif

		//
		// create the overlapped pipe
		//
		HANDLE hServer = ::CreateNamedPipe(
			CFB_PIPE_NAME,
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_ACCEPT_REMOTE_CLIENTS | PIPE_WAIT,
			CFB_PIPE_MAXCLIENTS,
			CFB_PIPE_INBUFLEN,
			CFB_PIPE_OUTBUFLEN,
			0,
			&SecurityAttributes
		);

		if (hServer == INVALID_HANDLE_VALUE)
		{
			PrintErrorWithFunctionName(L"CreateNamedPipe()");
			fSuccess = FALSE;
			break;
		}

		m_Transport.m_hServer = hServer;

		m_Transport.m_oOverlap.hEvent = ::CreateEvent(nullptr, FALSE, TRUE, nullptr);
		if (!m_Transport.m_oOverlap.hEvent)
		{
			xlog(LOG_CRITICAL, L"failed to create an event object for frontend thread\n");
			return false;
		}


		//
		// last, connect to the named pipe
		//
		fSuccess = ConnectPipe();
	} 
	while (false);

	if (pEveryoneSid)
		FreeSid(pEveryoneSid);

	if (pNewAcl)
		LocalFree(pNewAcl);


	return fSuccess;
}


/*++
Routine Description:

Destroys the FrontEndServer object.


Arguments:

	None


Return Value:
	May throw an exception if the the deallocation failed.

--*/
FrontEndServer::~FrontEndServer() noexcept(false)
{
	if (!ClosePipe())
	{
		RAISE_GENERIC_EXCEPTION("ClosePipe() failed");
	}
}


/*++

Routine Description:

Flush all the data and close the pipe.


Arguments:

	None


Return Value:
	Returns TRUE upon successful termination of the pipe, FALSE if any error occured.

--*/
BOOL FrontEndServer::ClosePipe()
{
#ifdef _DEBUG
	xlog(LOG_DEBUG, L"Closing named pipe '%s'...\n", CFB_PIPE_NAME);
#endif

	BOOL fSuccess = TRUE;
	HANDLE hServer = m_Transport.m_hServer;
	ServerState State = m_Transport.m_dwServerState;

	do
	{
		if (hServer == INVALID_HANDLE_VALUE)
		{
			//
			// already closed
			//
			break;
		}

		if (State != ServerState::Disconnected)
		{
			//
			// Wait until all data was consumed
			//
			if (!::FlushFileBuffers(hServer))
			{
				PrintErrorWithFunctionName(L"FlushFileBuffers()");
				fSuccess = FALSE;
			}

			//
			// Then close down the named pipe
			//
			if (!::DisconnectNamedPipe(hServer))
			{
				PrintErrorWithFunctionName(L"DisconnectNamedPipe()");
				fSuccess = FALSE;
			}
		}

		fSuccess = ::CloseHandle(hServer);
		m_Transport.m_hServer = INVALID_HANDLE_VALUE;
	} 
	while (false);

	return fSuccess;
}


/*++

Routine Description:


Arguments:


Return Value:

--*/
BOOL FrontEndServer::DisconnectAndReconnectPipe()
{

	if (!DisconnectNamedPipe(m_Transport.m_hServer))
	{
		PrintErrorWithFunctionName(L"DisconnectNamedPipe");
	}

	if (!ConnectPipe())
	{
		xlog(LOG_ERROR, L"error in ConnectPipe()\n");
		return false;
	}

	return true;
}


/*++

Routine Description:


Arguments:


Return Value:
	
	Returns TRUE on success, FALSE otherwise
--*/
BOOL FrontEndServer::ConnectPipe()
{
	HANDLE hServer = m_Transport.m_hServer;
	LPOVERLAPPED ov = &m_Transport.m_oOverlap;

	BOOL fSuccess = ::ConnectNamedPipe(hServer, ov);
	if (fSuccess)
	{
		PrintErrorWithFunctionName(L"ConnectNamedPipe");
		::DisconnectNamedPipe(hServer);
		return FALSE;
	}

	DWORD gle = ::GetLastError();

	switch (gle)
	{
	case ERROR_IO_PENDING:
		m_Transport.m_fPendingIo = TRUE;
		m_Transport.m_dwServerState = ServerState::Connecting;
		break;

	case ERROR_PIPE_CONNECTED:
		m_Transport.m_dwServerState = ServerState::ReadyToReadFromClient;
		::SetEvent(ov->hEvent);
		break;

	default:
		m_Transport.m_dwServerState = ServerState::Disconnected;
		xlog(LOG_ERROR, L"ConnectNamedPipe failed with %d.\n", gle);
		fSuccess = false;
		break;
	}

	return TRUE;
}


/*++

Routine Description:


Arguments:
	
	Session -


Return Value:

	Returns 0 on success, -1 on failure.

--*/
DWORD SendInterceptedIrps(_In_ Session& Session)
{
	HANDLE hServer = Session.FrontEndServer.m_Transport.m_hServer;

	json j = {
		{"header", {
			{"success", true},
			{"gle", ERROR_SUCCESS}
		}
	} };

	//
	// Make sure no element are being added concurrently
	//
	std::unique_lock<std::mutex> mlock(Session.m_IrpMutex);
	size_t i = 0;

	j["body"]["entries"] = json::array();

	while(!Session.m_IrpQueue.empty())
	{
		//
		// pop an IRP
		//
		Irp irp(Session.m_IrpQueue.front());
		Session.m_IrpQueue.pop();

		//
		// format a new JSON entry
		//
		j["body"]["entries"].push_back(irp.AsJson());


		//
		// The IRP is ready to be deleted
		//
		irp.Dispose();

		i++;
	}

	mlock.unlock();

	j["body"]["nb_entries"] = i;


	//
	// Write the data back
	//

	DWORD dwNbByteWritten;
	std::string data = j.dump();
	BOOL fSuccess = ::WriteFile(hServer, data.c_str(), data.size(), &dwNbByteWritten, NULL);

	if (!fSuccess)
	{
		PrintErrorWithFunctionName(L"WriteFile(hDevice)");
		return ERROR_INVALID_DATA;
	}
	/*
	std::string result(j.dump().c_str());
	DWORD dwNbByteWritten;
	DWORD dwBufferSize = (DWORD)result.length() + 3 * sizeof(uint32_t);
	byte* buf = new byte[dwBufferSize];
	uint32_t* p = (uint32_t*)buf;
	p[0] = TaskType::IoctlResponse;
	p[1] = (uint32_t)( ((DWORD)result.length()) + 2 * sizeof(uint32_t) );
	p[2] = ::GetLastError();
	::memcpy(&p[3], result.c_str(), result.length());


	//
	// Sync write back the whole JSON message
	//
	BOOL fSuccess = ::WriteFile(
		hServer,
		buf,
		dwBufferSize,
		&dwNbByteWritten,
		NULL
	);

	delete[] buf;

	if (!fSuccess)
	{
		PrintErrorWithFunctionName(L"WriteFile(hDevice)");
		return ERROR_INVALID_DATA;
	}
	*/
	return ERROR_SUCCESS;
}



/*++

Routine Description:


Arguments:

	Session -


Return Value:

	Returns 0 on success, -1 on failure.

--*/
DWORD SendDriverList(_In_ Session& Session)
{
	HANDLE hServer = Session.FrontEndServer.m_Transport.m_hServer;
	int i=0;
	
	json j = {
		{"header", {
			{"success", true},
			{"gle", ERROR_SUCCESS}
		}
	}};


	j["body"]["drivers"] = json::array();
	
	// wstring -> string converter
	std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> wide_converter;

	for (auto driver : Utils::EnumerateObjectDirectory(L"\\driver"))
	{
		std::string driver_name = wide_converter.to_bytes(driver.first);
		j["body"]["drivers"].push_back(driver_name);
		i++;
	}

	DWORD dwNbByteWritten;
	std::string data = j.dump();
	BOOL fSuccess = ::WriteFile(hServer, data.c_str(), data.size(), &dwNbByteWritten, NULL);

	if (!fSuccess)
	{
		PrintErrorWithFunctionName(L"WriteFile(hDevice)");
		return ERROR_INVALID_DATA;
	}

	return ERROR_SUCCESS;
}


/*++

Routine Description:

This routine handles the communication with the front-end of CFB (for now, the only one implemented
is the GUI).

Once a message from the frontend is received, it is parsed and pushed as an incoming Task, and notify
the BackEnd driver thread, then wait for an event from that same thread, notifying a response. Once the
response is popped from the outgoing Task list, the data is sent back to the frontend.


Arguments:

	lpParameter - the thread parameter


Return Value:

	Returns 0 the thread execution went successfully, the value from GetLastError() otherwise.

--*/
#define MAX_ACCEPTABLE_MESSAGE_SIZE 65534
#define MAX_MESSAGE_SIZE (MAX_ACCEPTABLE_MESSAGE_SIZE+2)


DWORD FrontendConnectionHandlingThread(_In_ LPVOID lpParameter)
{
	Session& Sess = *(reinterpret_cast<Session*>(lpParameter));
	DWORD dwNumberOfBytesWritten, dwIndexObject, cbRet;
	DWORD retcode = ERROR_SUCCESS;
	HANDLE hServerEvent = Sess.FrontEndServer.m_Transport.m_oOverlap.hEvent;
	HANDLE hServer = Sess.FrontEndServer.m_Transport.m_hServer;
	HANDLE hTermEvent = Sess.m_hTerminationEvent;
	HANDLE hResEvent = Sess.ResponseTasks.m_hPushEvent;
	BOOL fSuccess;

	const HANDLE Handles[4] = {
		hTermEvent,
		hServerEvent,
		hResEvent,
		hServer
	};

	PCHAR lpRequest = (PCHAR)::LocalAlloc(LPTR, MAX_MESSAGE_SIZE);
	if (!lpRequest)
		return ::GetLastError();
	DWORD dwRequestSize;

	while (Sess.IsRunning())
	{
		//
		// Wait for the pipe to be written to, or a termination notification event
		//

		dwIndexObject = ::WaitForMultipleObjects(_countof(Handles), Handles, FALSE, INFINITE) - WAIT_OBJECT_0;

		if (dwIndexObject < 0 || dwIndexObject >= _countof(Handles))
		{
			PrintErrorWithFunctionName(L"WaitForMultipleObjects()");
			xlog(LOG_CRITICAL, L"WaitForMultipleObjects(FrontEnd) has failed, cannot proceed...\n");
			Sess.Stop();
			continue;
		}


		//
		// if we received a termination event, stop everything
		//
		if (dwIndexObject == 0)
		{
#ifdef _DEBUG
			xlog(LOG_DEBUG, L"received termination Event\n");
#endif // _DEBUG
			Sess.Stop();
			continue;
		}
	

		//
		// otherwise, start by checking for pending IOs and update the state if needed
		//

		if (Sess.FrontEndServer.m_Transport.m_dwServerState == ServerState::Connecting)
		{
			LPOVERLAPPED ov = &Sess.FrontEndServer.m_Transport.m_oOverlap;
			fSuccess = ::GetOverlappedResult(hServer, ov, &cbRet, FALSE);

			if (!fSuccess)
			{
				//
				// assume the connection has closed
				//
				Sess.FrontEndServer.DisconnectAndReconnectPipe();
				continue;
			}

#ifdef _DEBUG
			xlog(LOG_DEBUG, L"new pipe connection\n");
#endif // _DEBUG

			Sess.FrontEndServer.m_Transport.m_dwServerState = ServerState::ReadyToReadFromClient;

		}


		//
		// process the state itself
		//	
		ServerState State = Sess.FrontEndServer.m_Transport.m_dwServerState;
		
		if (State == ServerState::ReadyToReadFromClient)
		{
			RtlZeroMemory(lpRequest, MAX_MESSAGE_SIZE);

			try
			{
				//
				// read the json message
				//
				
				HANDLE Handle = Sess.FrontEndServer.m_Transport.m_hServer;
				BOOL fSuccess = ::ReadFile(Handle, lpRequest, MAX_ACCEPTABLE_MESSAGE_SIZE, &dwRequestSize, NULL);
				if (!fSuccess)
					RAISE_GENERIC_EXCEPTION("ReadFile() failed");

#ifdef _DEBUG
				dbg(L"new pipe message (len=%d)\n", dwRequestSize);
				hexdump(lpRequest, dwRequestSize);
#endif // _DEBUG

				auto json_request = json::parse(std::string(lpRequest));
				TaskType type = static_cast<TaskType>(json_request["body"]["type"]);
				DWORD dwDataLength = json_request["body"]["data_length"];
				auto data = json_request["body"]["data"].get<std::string>();
				auto lpData = Utils::base64_decode(data);

				assert(lpData.size() == dwDataLength);

				//
				// build a Task object from the next message read from the pipe
				//
				Task task(type, lpData.data(), dwDataLength, -1);


				dbg(L"new request task (id=%d, type='%s', length=%d)\n", task.Id(), task.TypeAsString(), task.Length());

				switch (task.Type())
				{
				case TaskType::GetInterceptedIrps:
					SendInterceptedIrps(Sess);
					continue;

				case TaskType::EnumerateDrivers:
					SendDriverList(Sess);
					continue;

				case TaskType::ReplayIrp:
					//
					// Replay the IRP
					//
					// TODO
				default:

					// push the task to request task list
					Sess.RequestTasks.push(task);

					// change the state
					Sess.FrontEndServer.m_Transport.m_dwServerState = ServerState::ReadyToReadFromServer;
				}
			}
			catch (BrokenPipeException&)
			{
				xlog(LOG_WARNING, L"Broken pipe detected...\n");
				Sess.FrontEndServer.DisconnectAndReconnectPipe();
				continue;
			}
			catch (BaseException &e)
			{
				xlog(LOG_ERROR, L"An exception occured while processing incoming message:\n%S\n", e.what());
				Sess.FrontEndServer.DisconnectAndReconnectPipe();
				continue;
			}

		}
		else if (State == ServerState::ReadyToReadFromServer)
		{
			try
			{

				//
				// pop the response task and build the json message
				//
				auto task = Sess.ResponseTasks.pop();

				dbg(L"new response task (id=%d, type='%s', length=%d, gle=%d)\n", task.Id(), task.TypeAsString(), task.Length(), task.ErrCode());

				json json_response = {
					{"header", {
						{"success", task.ErrCode()==ERROR_SUCCESS},
						{"gle", task.ErrCode()},
					}
				} };

				json_response["body"]["data_length"] = task.Length();
				if (task.Length() > 0)
					json_response["body"]["data"] = Utils::base64_encode(task.Data(), task.Length());

				std::string data = json_response.dump();
				BOOL fSuccess = ::WriteFile(hServer, data.c_str(), data.size(), &dwNumberOfBytesWritten, NULL);
				if (!fSuccess)
				{
					PrintErrorWithFunctionName(L"WriteFile(hDevice)");
				}
				else
				{
					task.SetState(TaskState::Completed);
				}


#ifdef _DEBUG
				xlog(LOG_DEBUG, L"task tid=%d sent to frontend (%dB), terminating...\n", task.Id(), dwNumberOfBytesWritten);
#endif // _DEBUG

				// change the state
				Sess.FrontEndServer.m_Transport.m_dwServerState = ServerState::ReadyToReadFromClient;
			}
			catch (BrokenPipeException&)
			{
				xlog(LOG_WARNING, L"Broken pipe detected...\n");
				Sess.FrontEndServer.DisconnectAndReconnectPipe();
				continue;
			}
			catch (BaseException & e)
			{
				xlog(LOG_ERROR, L"An exception occured while processing incoming message:\n%S\n", e.what());
				continue;
			}
		}
		else
		{
			xlog(LOG_WARNING, L"Unexpected state (state=%d, fd=%d), invalid?\n", State, dwIndexObject);
			Sess.FrontEndServer.DisconnectAndReconnectPipe();
		}

	}

	LocalFree(lpRequest);

#ifdef _DEBUG
	xlog(LOG_DEBUG, L"terminating thread TID=%d\n", GetThreadId(GetCurrentThread()));
#endif // _DEBUG

	return ERROR_SUCCESS;
}



/*++

Routine Description:

This function is a simple wrapper around CreateThread() to start the thread handling the conversation
with the frontend part of the application.


Arguments:

	lpParameter - a generic pointer to the global Session


Return Value:
	Returns TRUE upon successful creation of the thread, FALSE if any error occured.

--*/
_Success_(return)
BOOL StartFrontendManagerThread(_In_ LPVOID lpParameter)
{
	DWORD dwThreadId;

	HANDLE hThread = ::CreateThread(
		NULL,
		0,
		FrontendConnectionHandlingThread,
		lpParameter,
		CREATE_SUSPENDED,
		&dwThreadId
	);

	if (!hThread)
	{
		PrintErrorWithFunctionName(L"CreateThread(hThreadPipeIn)");
		return FALSE;
	}


#ifdef _DEBUG
	xlog(LOG_DEBUG, "Created frontend thread as TID=%d\n", dwThreadId);
#endif

	Session& Sess = *(reinterpret_cast<Session*>(lpParameter));
	Sess.m_hFrontendThread = hThread;

	return TRUE;
}