/*
 *      Copyright (C) 2018 Aassif Benassarou
 *      http://github.com/aassif/pvr.freebox/
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "client.h"
#include "kodi/xbmc_pvr_dll.h"
#include "Freebox.h"
#include "p8-platform/util/util.h"

using namespace ADDON;

#ifdef TARGET_WINDOWS
#define snprintf _snprintf
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#endif

#define PVR_FREEBOX_DEFAULT_SERVER   "mafreebox.freebox.fr"
#define PVR_FREEBOX_DEFAULT_DELAY    10
#define PVR_FREEBOX_DEFAULT_SOURCE   1
#define PVR_FREEBOX_DEFAULT_QUALITY  1
#define PVR_FREEBOX_DEFAULT_EXTENDED false
#define PVR_FREEBOX_DEFAULT_COLORS   false

std::string  path;
std::string  server   = PVR_FREEBOX_DEFAULT_SERVER;
int          delay    = PVR_FREEBOX_DEFAULT_DELAY;
int          source   = PVR_FREEBOX_DEFAULT_SOURCE;
int          quality  = PVR_FREEBOX_DEFAULT_QUALITY;
bool         extended = PVR_FREEBOX_DEFAULT_EXTENDED;
bool         colors   = PVR_FREEBOX_DEFAULT_COLORS;
bool         init     = false;
ADDON_STATUS status   = ADDON_STATUS_UNKNOWN;
Freebox    * data     = nullptr;

CHelper_libXBMC_addon  * XBMC = nullptr;
CHelper_libXBMC_pvr    * PVR  = nullptr;
CHelper_libKODI_guilib * GUI  = nullptr;

extern "C" {

void ADDON_ReadSettings ()
{
  char buffer [256];
  server = XBMC->GetSetting ("server", buffer) ? buffer : PVR_FREEBOX_DEFAULT_SERVER;

  if (! XBMC->GetSetting ("delay",    &delay))    delay    = PVR_FREEBOX_DEFAULT_DELAY;
  if (! XBMC->GetSetting ("source",   &source))   source   = PVR_FREEBOX_DEFAULT_SOURCE;
  if (! XBMC->GetSetting ("quality",  &quality))  quality  = PVR_FREEBOX_DEFAULT_QUALITY;
  if (! XBMC->GetSetting ("extended", &extended)) extended = PVR_FREEBOX_DEFAULT_EXTENDED;
  if (! XBMC->GetSetting ("colors",   &colors))   colors   = PVR_FREEBOX_DEFAULT_COLORS;
}

ADDON_STATUS ADDON_Create (void * callbacks, void * properties)
{
  if (! callbacks || ! properties)
  {
    return ADDON_STATUS_UNKNOWN;
  }

  PVR_PROPERTIES * p = (PVR_PROPERTIES *) properties;

  XBMC = new CHelper_libXBMC_addon;
  if (! XBMC->RegisterMe (callbacks))
  {
    SAFE_DELETE (XBMC);
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  PVR = new CHelper_libXBMC_pvr;
  if (! PVR->RegisterMe (callbacks))
  {
    SAFE_DELETE (PVR);
    SAFE_DELETE (XBMC);
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  GUI = new CHelper_libKODI_guilib;
  if (! GUI->RegisterMe (callbacks))
  {
    SAFE_DELETE (GUI);
    SAFE_DELETE (PVR);
    SAFE_DELETE (XBMC);
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  XBMC->Log (LOG_DEBUG, "%s - Creating the Freebox TV add-on", __FUNCTION__);

  status = ADDON_STATUS_UNKNOWN;

  if (! XBMC->DirectoryExists (p->strUserPath))
    XBMC->CreateDirectory (p->strUserPath);

  ADDON_ReadSettings ();

  static std::vector<PVR_MENUHOOK> HOOKS =
  {
    {PVR_FREEBOX_MENUHOOK_CHANNEL_SOURCE,  PVR_FREEBOX_STRING_CHANNEL_SOURCE,  PVR_MENUHOOK_CHANNEL},
    {PVR_FREEBOX_MENUHOOK_CHANNEL_QUALITY, PVR_FREEBOX_STRING_CHANNEL_QUALITY, PVR_MENUHOOK_CHANNEL}
  };

  for (PVR_MENUHOOK & h : HOOKS)
    PVR->AddMenuHook (&h);

  data   = new Freebox (p->strUserPath, server, source, quality, p->iEpgMaxDays, extended, colors, delay);
  status = ADDON_STATUS_OK;
  init   = true;

  return status;
}

ADDON_STATUS ADDON_GetStatus ()
{
  return status;
}

void ADDON_Destroy ()
{
  delete data;
  status = ADDON_STATUS_UNKNOWN;
  init   = false;
}

ADDON_STATUS ADDON_SetSetting (const char * name, const void * value)
{
  if (data)
  {
    if (! strcmp (name, "server"))
    {
      data->SetServer ((char *) value);
      return ADDON_STATUS_NEED_RESTART;
    }

    if (! strcmp (name, "delay"))
      data->SetDelay (*((int *) value));

    if (! strcmp (name, "restart"))
    {
      bool restart = *((bool *) value);
      return restart ? ADDON_STATUS_NEED_RESTART : ADDON_STATUS_OK;
    }

    if (! strcmp (name, "source"))
      data->SetSource (*((int *) value));

    if (! strcmp (name, "quality"))
      data->SetQuality (*((int *) value));

    if (! strcmp (name, "extended"))
      data->SetExtended (*((bool *) value));

    if (! strcmp (name, "colors"))
    {
      data->SetColors (*((bool *) value));
      return ADDON_STATUS_NEED_RESTART;
    }
  }

  return ADDON_STATUS_OK;
}

/***********************************************************
 * PVR Client AddOn specific public library functions
 ***********************************************************/

void OnSystemSleep ()
{
}

void OnSystemWake ()
{
}

void OnPowerSavingActivated ()
{
}

void OnPowerSavingDeactivated ()
{
}

PVR_ERROR GetAddonCapabilities (PVR_ADDON_CAPABILITIES * caps)
{
  caps->bSupportsEPG                      = true;
  caps->bSupportsTV                       = true;
  caps->bSupportsRadio                    = false;
  caps->bSupportsChannelGroups            = false;
  caps->bSupportsRecordings               = true;
  caps->bSupportsRecordingSize            = true;
  caps->bSupportsRecordingsRename         = true;
  caps->bSupportsRecordingsUndelete       = false;
  caps->bSupportsRecordingsLifetimeChange = false;
  caps->bSupportsTimers                   = true;
  caps->bSupportsDescrambleInfo           = false;
  caps->bSupportsAsyncEPGTransfer         = true;

  return PVR_ERROR_NO_ERROR;
}

const char * GetBackendName ()
{
  return PVR_FREEBOX_BACKEND_NAME;
}

const char * GetBackendVersion ()
{
  return PVR_FREEBOX_BACKEND_VERSION;
}

const char * GetConnectionString ()
{
  return PVR_FREEBOX_CONNECTION_STRING;
}

const char * GetBackendHostname ()
{
  if (data)
  {
    static std::string server;
    server = data->GetServer ();
    return server.c_str ();
  }

  return NULL;
}

PVR_ERROR SetEPGTimeFrame (int days)
{
  if (data)
    data->SetDays (days);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR GetEPGForChannel (ADDON_HANDLE handle, int id, time_t start, time_t end)
{
  return PVR_ERROR_NO_ERROR;
}

int GetChannelsAmount ()
{
  return data ? data->GetChannelsAmount () : -1;
}

PVR_ERROR GetChannels (ADDON_HANDLE handle, bool radio)
{
  return data ? data->GetChannels (handle, radio) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR GetChannelStreamProperties (const PVR_CHANNEL * channel, PVR_NAMED_VALUE * properties, unsigned int * count)
{
  return data ? data->GetChannelStreamProperties (channel, properties, count) : PVR_ERROR_SERVER_ERROR;
}

int GetChannelGroupsAmount ()
{
  return data ? data->GetChannelGroupsAmount () : -1;
}

PVR_ERROR GetChannelGroups (ADDON_HANDLE handle, bool radio)
{
  return data ? data->GetChannelGroups (handle, radio) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR GetChannelGroupMembers (ADDON_HANDLE handle, const PVR_CHANNEL_GROUP & group)
{
  return data ? data->GetChannelGroupMembers (handle, group) : PVR_ERROR_SERVER_ERROR;
}

int GetRecordingsAmount (bool deleted)
{
  return data ? data->GetRecordingsAmount (deleted) : -1;
}

PVR_ERROR GetRecordings (ADDON_HANDLE handle, bool deleted)
{
  return data ? data->GetRecordings (handle, deleted) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR GetRecordingSize (const PVR_RECORDING * recording, int64_t * size)
{
  return data ? data->GetRecordingSize (recording, size) : PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR GetRecordingStreamProperties (const PVR_RECORDING * recording, PVR_NAMED_VALUE * properties, unsigned int * count)
{
  return data ? data->GetRecordingStreamProperties (recording, properties, count) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR RenameRecording (const PVR_RECORDING & recording)
{
  return data ? data->RenameRecording (recording) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR DeleteRecording (const PVR_RECORDING & recording)
{
  return data ? data->DeleteRecording (recording) : PVR_ERROR_SERVER_ERROR;
}

int GetTimersAmount ()
{
  return data ? data->GetTimersAmount () : -1;
}

PVR_ERROR GetTimers (ADDON_HANDLE handle)
{
  return data ? data->GetTimers (handle) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR GetTimerTypes (PVR_TIMER_TYPE types [], int * size)
{
  return data ? data->GetTimerTypes (types, size) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR AddTimer (const PVR_TIMER & timer)
{
  return data ? data->AddTimer (timer) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR UpdateTimer (const PVR_TIMER & timer)
{
  return data ? data->UpdateTimer (timer) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR DeleteTimer (const PVR_TIMER & timer, bool force)
{
  return data ? data->DeleteTimer (timer, force) : PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR GetDriveSpace (long long *, long long *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR SignalStatus (PVR_SIGNAL_STATUS &) {return PVR_ERROR_NOT_IMPLEMENTED;}

PVR_ERROR CallMenuHook (const PVR_MENUHOOK & hook, const PVR_MENUHOOK_DATA & d)
{
  return data ? data->MenuHook (hook, d) : PVR_ERROR_SERVER_ERROR;
}

/** UNUSED API FUNCTIONS */
bool CanPauseStream () {return false;}
PVR_ERROR OpenDialogChannelScan () {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR DeleteChannel (const PVR_CHANNEL &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR RenameChannel (const PVR_CHANNEL &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR OpenDialogChannelSettings (const PVR_CHANNEL &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR OpenDialogChannelAdd (const PVR_CHANNEL &) {return PVR_ERROR_NOT_IMPLEMENTED;}
void CloseLiveStream () {}
bool OpenRecordedStream (const PVR_RECORDING &) {return false;}
bool OpenLiveStream (const PVR_CHANNEL &) {return false;}
void CloseRecordedStream () {}
int ReadRecordedStream (unsigned char *, unsigned int) {return 0;}
long long SeekRecordedStream (long long, int) {return 0;}
long long LengthRecordedStream () {return 0;}
void DemuxReset () {}
void DemuxFlush () {}
void FillBuffer (bool) {}
int ReadLiveStream (unsigned char *, unsigned int) {return 0;}
long long SeekLiveStream (long long, int) {return -1;}
long long LengthLiveStream () {return -1;}
PVR_ERROR SetRecordingPlayCount (const PVR_RECORDING &, int) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR SetRecordingLastPlayedPosition (const PVR_RECORDING &, int) {return PVR_ERROR_NOT_IMPLEMENTED;}
int GetRecordingLastPlayedPosition (const PVR_RECORDING &) {return -1;}
PVR_ERROR GetRecordingEdl (const PVR_RECORDING &, PVR_EDL_ENTRY [], int *) {return PVR_ERROR_NOT_IMPLEMENTED;};
void DemuxAbort () {}
bool IsTimeshifting () {return false;}
DemuxPacket * DemuxRead () {return NULL;}
bool IsRealTimeStream () {return true;}
void PauseStream (bool) {}
bool CanSeekStream () {return false;}
bool SeekTime (double, bool, double *) {return false;}
void SetSpeed (int) {};
PVR_ERROR UndeleteRecording (const PVR_RECORDING &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR DeleteAllRecordingsFromTrash () {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetDescrambleInfo (PVR_DESCRAMBLE_INFO *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR SetRecordingLifetime (const PVR_RECORDING *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetStreamTimes (PVR_STREAM_TIMES *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetStreamProperties (PVR_STREAM_PROPERTIES *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR IsEPGTagRecordable (const EPG_TAG *, bool *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR IsEPGTagPlayable (const EPG_TAG *, bool *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetEPGTagStreamProperties (const EPG_TAG *, PVR_NAMED_VALUE *, unsigned int *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetEPGTagEdl (const EPG_TAG *, PVR_EDL_ENTRY [], int *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetStreamReadChunkSize (int *) {return PVR_ERROR_NOT_IMPLEMENTED;}

} // extern "C"
