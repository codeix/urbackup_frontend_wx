/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2015 Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "Connector.h"
#include "stringtools.h"
#include "escape.h"
#include <iostream>
#include "json/json.h"
#include <stdexcept>

#ifndef _WIN32
#include "../config.h"
#endif

std::string Connector::pw;
std::string Connector::pw_change;
bool Connector::error=false;
bool Connector::busy=false;

extern std::string g_res_path;

#ifdef wxUSE_WCHAR_T
std::string ConvertToUTF8(const std::wstring &str);
std::wstring ConvertToUnicode(const std::string &str);
#else
#define std_string std::string
#define ConvertToUTF8(x)
#define ConvertToUnicode(x)
#endif

namespace
{
	class ScopedSocket
	{
	public:
		ScopedSocket(wxSocketClient* socket_client)
			:socket_client(socket_client) {}

		~ScopedSocket()
		{
			if (socket_client != NULL)
			{
				socket_client->Destroy();
			}
		}

		void release()
		{
			socket_client = NULL;
		}
	private:
		wxSocketClient* socket_client;
	};
}



std::string Connector::getPasswordData( bool change_command, bool set_busy)
{
	const std::string pw_fn = change_command ? "pw_change.txt" : "pw.txt";

	if(set_busy)
	{
		busy = true;
	}
	
	error=false;
	std::string curr_pw;

	if(change_command)
		curr_pw = pw_change;
	else
		curr_pw = pw;

	if( curr_pw.empty())
	{
#ifdef _DEBUG
		if(FileExists(pw_fn))
			curr_pw=getFile(pw_fn);
		else
#endif
		{
#ifdef _WIN32
			curr_pw = getFile(g_res_path + pw_fn);
#else
			curr_pw = getFile(VARDIR "/urbackup/" + pw_fn);
#endif
		}

		if(curr_pw.empty())
		{
			std::cout << "Could not load password file!" << std::endl;
		}
		else
		{
			if(change_command)
				pw_change = curr_pw;
			else
				pw = curr_pw;
		}
	}

	return curr_pw;
}


std::string Connector::getResponse(const std::string &cmd, const std::string &args, bool change_command, SConnection* connection, size_t timeoutms, bool set_busy)
{
	wxLongLong starttime = wxGetLocalTimeMillis();

	std::string curr_pw = getPasswordData(change_command, set_busy);

	wxSocketClient* client;
	if (connection == NULL
		|| connection->client == NULL
		|| connection->client->Error())
	{
		if (connection != NULL
			&& connection->client != NULL)
		{
			connection->client->Destroy();
			connection->client = NULL;
		}

		client = new wxSocketClient(wxSOCKET_BLOCK);

		wxIPV4address addr;
		addr.Hostname(wxT("127.0.0.1"));
		addr.Service(35623);
		if (!client->Connect(addr, false) )
		{
			while (wxGetLocalTimeMillis() - starttime<timeoutms)
			{
				if (client->WaitOnConnect(0, 100))
				{
					break;
				}
				wxTheApp->Yield(true);
			}
		}

		if (!client->IsConnected())
		{
			client->Destroy();
			error = true;
			busy = false;
			return "";
		}
	}
	else
	{
		client = connection->client;
		connection->client = NULL;
	}

	ScopedSocket client_socket(client);

	std::string t_args;

	if(!args.empty())
		t_args="&"+args;
	else
		t_args=args;

	CTCPStack tcpstack;
	tcpstack.Send(client, cmd+"#pw="+curr_pw+t_args);

	char *resp=NULL;
	char buffer[1024];
	size_t packetsize;
	while(resp==NULL)
	{
		bool conn=false;
		while(wxGetLocalTimeMillis()-starttime<timeoutms)
		{
			if(client->WaitForRead(0, 100))
			{
				conn=true;
				break;
			}
			wxTheApp->Yield(true);
			if(client->Error())
				break;
		}
		if(!conn)
		{
			error=true;
			busy=false;
			return "";
		}
		client->Read(buffer, 1024);
		
		if(client->Error())
		{
			error=true;
			busy=false;
			return "";
		}
		tcpstack.AddData(buffer, client->GetLastIOSize() );
		

		resp=tcpstack.getPacket(&packetsize);
		if(packetsize==0)
		{
			busy=false;
			return "";
		}
	}

	std::string ret;
	ret.resize(packetsize);
	memcpy(&ret[0], resp, packetsize);
	delete resp;

	busy=false;

	if (connection != NULL)
	{
		connection->client = client;
		client_socket.release();
	}

	return ret;
}

bool Connector::hasError(void)
{
	return error;
}

std::vector<SBackupDir> Connector::getSharedPaths(void)
{
	std::vector<SBackupDir> ret;
	std::string d=getResponse("GET BACKUP DIRS","", false);

	if (d.empty())
	{
		error = true;
		return ret;
	}

	Json::Value root;
	Json::Reader reader;

	if(!reader.parse(d, root, false))
	{
		return ret;
	}

	try
	{
		Json::Value dirs = root["dirs"];

		for(Json::Value::ArrayIndex i=0;i<dirs.size();++i)
		{
			Json::Value dir = dirs[i];

			if (!dir.get("virtual_client", Json::nullValue).isNull())
			{
				continue;
			}

			SBackupDir rdir =
			{
				wxString::FromUTF8(dir["path"].asCString()),
				wxString::FromUTF8(dir["name"].asCString()),
				dir["id"].asInt(),
				dir["group"].asInt(),
				wxString::FromUTF8(dir["flags"].asCString())
			};

			ret.push_back(rdir);
		}
	}
	catch(std::runtime_error&)
	{
	}

	return ret;
}

bool Connector::saveSharedPaths(const std::vector<SBackupDir> &res)
{
	std::string args;
	for(size_t i=0;i<res.size();++i)
	{
		if(i!=0)
			args+="&";

		std::string path = EscapeParamString(res[i].path.ToUTF8().data());
		std::string name = EscapeParamString(res[i].name.ToUTF8().data());

		if (name.find("/") == std::string::npos
			&& !res[i].flags.empty())
		{
			name += "/" + res[i].flags;
		}

		args+="dir_"+nconvert(i)+"="+path;
		args+="&dir_"+nconvert(i)+"_name="+name;
		args+="&dir_"+nconvert(i)+"_group="+nconvert(res[i].group);
	}

	std::string d=getResponse("SAVE BACKUP DIRS", args, true);

	if(d!="OK")
		return false;
	else
		return true;
}

int Connector::startBackup(bool full)
{
	std::string s;
	if(full)
		s="START BACKUP FULL";
	else
		s="START BACKUP INCR";

	std::string d=getResponse(s,"", false);

	if(d=="RUNNING")
		return 2;
	else if(d=="NO SERVER")
		return 3;
	else if(d!="OK")
		return 0;
	else
		return 1;
}

int Connector::startImage(bool full)
{
	std::string s;
	if(full)
		s="START IMAGE FULL";
	else
		s="START IMAGE INCR";

	std::string d=getResponse(s,"", false);

	if(d=="RUNNING")
		return 2;
	else if(d=="NO SERVER")
		return 3;
	else if(d!="OK")
		return 0;
	else
		return 1;
}

bool Connector::updateSettings(const std::string &ndata)
{
	std::string data=ndata;
	escapeClientMessage(data);
	std::string d=getResponse("UPDATE SETTINGS "+data,"", true);

	if(d!="OK")
		return false;
	else
		return true;
}

std::vector<SLogEntry> Connector::getLogEntries(void)
{
	std::string d=getResponse("GET LOGPOINTS", "", true);
	int lc=linecount(d);
	std::vector<SLogEntry> ret;
	for(int i=0;i<lc;++i)
	{
		std::string l=getline(i, d);
		if(l.empty())continue;
		SLogEntry le;
		le.logid=atoi(getuntil("-", l).c_str() );
		std::string lt=getafter("-", l);
		le.logtime=wxString::FromUTF8(lt.c_str());
		ret.push_back(le);
	}
	return ret;
}

std::vector<SLogLine>  Connector::getLogdata(int logid, int loglevel)
{
	std::string d=getResponse("GET LOGDATA","logid="+nconvert(logid)+"&loglevel="+nconvert(loglevel), true);
	std::vector<std::string> lines;
	TokenizeMail(d, lines, "\n");
	std::vector<SLogLine> ret;
	for(size_t i=0;i<lines.size();++i)
	{
		std::string l=lines[i];
		if(l.empty())continue;
		SLogLine ll;
		ll.loglevel=atoi(getuntil("-", l).c_str());
		ll.msg=wxString::FromUTF8(getafter("-", l).c_str());
		ret.push_back(ll);
	}
	return ret;
}

bool Connector::setPause(bool b_pause)
{
	std::string data=b_pause?"true":"false";
	std::string d=getResponse("PAUSE "+data,"", false);

	if(d!="OK")
		return false;
	else
		return true;
}

bool Connector::isBusy(void)
{
	return busy;
}

bool Connector::addNewServer(const std::string &ident)
{
	std::string d=getResponse("NEW SERVER","ident="+ident, true);

	if(d!="OK")
		return false;
	else
		return true;
}

SStatusDetails Connector::getStatusDetails(SConnection* connection)
{
	std::string d=getResponse("STATUS DETAIL","", false, connection);

	SStatusDetails ret;
	ret.ok=false;

	Json::Value root;
    Json::Reader reader;

	if(!reader.parse(d, root, false))
	{
		return ret;
	}

	try
	{
		ret.last_backup_time = root["last_backup_time"].asInt64();

		std::vector<SRunningProcess> running_processes;
		Json::Value json_running_processes = root["running_processes"];
		running_processes.resize(json_running_processes.size());
		for (unsigned int i = 0; i<json_running_processes.size(); ++i)
		{
			running_processes[i].action = json_running_processes[i]["action"].asString();
			running_processes[i].percent_done = json_running_processes[i]["percent_done"].asInt();
			running_processes[i].eta_ms = json_running_processes[i]["eta_ms"].asInt64();

			running_processes[i].details = json_running_processes[i].get("details", std::string()).asString();
			running_processes[i].detail_pc = json_running_processes[i].get("detail_pc", -1).asInt();

			running_processes[i].process_id = json_running_processes[i].get("process_id", 0).asInt64();
		}
		ret.running_processes = running_processes;

		std::vector<SUrBackupServer> servers;
		Json::Value json_servers = root["servers"];
		servers.resize(json_servers.size());
		for(unsigned int i=0;i<json_servers.size();++i)
		{
			servers[i].internet_connection = json_servers[i]["internet_connection"].asBool();
			servers[i].name = json_servers[i]["name"].asString();
		}
		ret.servers=servers;
		ret.time_since_last_lan_connection = root["time_since_last_lan_connection"].asInt64();
		ret.internet_connected = root["internet_connected"].asBool();
		ret.internet_status = root["internet_status"].asString();

		ret.capability_bits = root["capability_bits"].asInt();

		ret.ok=true;

		return ret;
	}
	catch (std::runtime_error&)
	{
		return ret;
	}
	
}

std::string Connector::getAccessParameters( const std::string& tokens )
{
	return getResponse("GET ACCESS PARAMETERS","tokens="+tokens, false);
}

int Connector::getCapabilities()
{
	SStatusDetails status = getStatusDetails();
	if(status.ok)
	{
		return status.capability_bits;
	}
	else
	{
		return 0;
	}
}

bool Connector::restoreOk( bool ok, wxLongLong_t& process_id)
{
	std::string d = getResponse("RESTORE OK", "ok="+nconvert(ok), false);

	Json::Value root;
	Json::Reader reader;

	if (!reader.parse(d, root, false))
	{
		return false;
	}

	if (root.get("accepted", false).asBool() == true)
	{
		process_id = root["process_id"].asInt64();
	}

	return root["ok"].asBool();
}

SStatus Connector::initStatus(wxSocketClient* last_client, bool fast, size_t timeoutms/*=5000*/ )
{
	if (last_client != NULL
		&& last_client->Error())
	{
		last_client->Destroy();
		last_client = NULL;
	}

	bool change_command=false;
	bool set_busy=false;
	std::string cmd;
	if(fast)
	{
		cmd = "FSTATUS";
	}
	else
	{
		cmd = "STATUS";
	}
	std::string args="";
	std::string curr_pw = getPasswordData(change_command, set_busy);

	SStatus status;
	status.init=false;
	status.starttime = wxGetLocalTimeMillis();
	if (last_client != NULL)
	{
		status.client = last_client;
	}
	else
	{
		status.client = new wxSocketClient(wxSOCKET_BLOCK);

		wxIPV4address addr;
		addr.Hostname(wxT("127.0.0.1"));
		addr.Service(35623);
		if (!status.client->Connect(addr, false))
		{
			while (wxGetLocalTimeMillis() - status.starttime<timeoutms)
			{
				if (status.client->WaitOnConnect(0, 100))
				{
					break;
				}
				wxTheApp->Yield(true);
			}
		}

		if (!status.client->IsConnected())
		{
			wxSocketError err = status.client->LastError();
			status.client->Destroy();
			status.client = NULL;
			error = true;
			busy = false;
			return status;
		}
	}

	std::string t_args;

	if(!args.empty())
		t_args="&"+args;
	else
		t_args=args;

	CTCPStack tcpstack;
	tcpstack.Send(status.client, cmd+"#pw="+curr_pw+t_args);
	
	status.timeoutms = timeoutms;
	status.init = true;
	return status;
}

bool SStatus::isAvailable()
{
	bool conn=false;
	if(wxGetLocalTimeMillis()-starttime<timeoutms)
	{
		if(!client->WaitForRead(0, 10))
		{
			return false;
		}
		if(client->Error())
		{
			error=true;
			client->Destroy();
			client = NULL;
			return false;
		}
	}
	else
	{
		error=true;
		client->Destroy();
		client = NULL;
		return false;
	}

	char buffer[1024];
	client->Read(buffer, 1024);

	if(client->Error())
	{
		error=true;
		client->Destroy();
		client = NULL;
		return false;
	}

	tcpstack.AddData(buffer, client->GetLastIOSize() );

	size_t packetsize;
	char* resp=tcpstack.getPacket(&packetsize);
	if(packetsize==0)
	{
		error=true;
		return false;
	}

	std::string ret;
	ret.resize(packetsize);
	memcpy(&ret[0], resp, packetsize);
	delete resp;

	std::vector<std::string> toks;
	Tokenize(ret, toks, "#");

	pause=false;
	capa=0;
	ask_restore_ok=false;
	if(toks.size()>0)
		lastbackupdate=wxString::FromUTF8(toks[0].c_str() );
	if(toks.size()>1)
		status=wxString::FromUTF8(toks[1].c_str() );
	if(toks.size()>2)
		pcdone=wxString::FromUTF8(toks[2].c_str() );
	if(toks.size()>3)
	{
		if(toks[3]=="P")
			pause=true;
		else if(toks[3]=="NP")
			pause=false;
	}
	if(toks.size()>4)
	{
		std::map<std::wstring,std::wstring> params;
		ParseParamStr(toks[4], &params);
		std::map<std::wstring,std::wstring>::iterator it_capa=params.find(L"capa");
		if(it_capa!=params.end())
		{
			capa=watoi(it_capa->second);
		}
		std::map<std::wstring,std::wstring>::iterator it_new_server=params.find(L"new_ident");
		if(it_new_server!=params.end())
		{
			new_server=wnarrow(it_new_server->second);
		}
		std::map<std::wstring,std::wstring>::iterator it_has_server=params.find(L"has_server");
		if(it_has_server!=params.end())
		{
			has_server= ( it_has_server->second==L"true" );
		}
		std::map<std::wstring,std::wstring>::iterator it_restore_ask=params.find(L"restore_ask");
		if(it_restore_ask!=params.end())
		{
			ask_restore_ok = watoi(it_restore_ask->second);
		}
		std::map<std::wstring, std::wstring>::iterator it_restore_file = params.find(L"restore_file");
		if (it_restore_file != params.end())
		{
			restore_file = (it_restore_file->second == L"true");
		}
		std::map<std::wstring, std::wstring>::iterator it_restore_path = params.find(L"restore_path");
		if (it_restore_path != params.end())
		{
			restore_path = it_restore_path->second;
		}
		std::map<std::wstring,std::wstring>::iterator it_needs_restore_restart=params.find(L"needs_restore_restart");
		if(it_needs_restore_restart!=params.end())
		{
			needs_restore_restart = watoi(it_needs_restore_restart->second);
		}
	}

	return true;
}

bool SStatus::hasError()
{
	return error;
}
