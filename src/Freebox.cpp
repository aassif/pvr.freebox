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

#define RAPIDJSON_HAS_STDSTRING 1

#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <filesystem>

#include "p8-platform/util/StringUtils.h"

#include "client.h"
#include "Freebox.h"

#include "curl/curl.h"
#include "openssl/sha.h"
#include "openssl/hmac.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/ostreamwrapper.h"

using namespace std;
using namespace rapidjson;
using namespace ADDON;

#define PVR_FREEBOX_C_STR(s) s.empty () ? NULL : s.c_str ()

#define PVR_FREEBOX_TIMER_MANUAL     1
#define PVR_FREEBOX_TIMER_EPG        2
#define PVR_FREEBOX_TIMER_GENERATED  3
#define PVR_FREEBOX_GENERATOR_MANUAL 4
#define PVR_FREEBOX_GENERATOR_EPG    5

void freebox_debug (const Value & data)
{
  OStreamWrapper wrapper (cout);
  Writer<OStreamWrapper> writer (wrapper);
  data.Accept (writer);
  cout << endl;
}

size_t freebox_http_write (char * ptr, size_t size, size_t nmemb, void * userdata)
{
  size_t realsize = size * nmemb;
  ((string *) userdata)->append (ptr, realsize);
  return realsize;
}

long freebox_http (const string & custom, const string & url, const string & request, string * response, const string & session)
{
  CURL * curl = curl_easy_init ();
  // URL.
  curl_easy_setopt (curl, CURLOPT_URL, url.c_str ());
  // Custom request.
  curl_easy_setopt (curl, CURLOPT_CUSTOMREQUEST, custom.c_str ());
  // Response handler.
  curl_easy_setopt (curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, freebox_http_write);
  // Header.
  string header = "X-Fbx-App-Auth: " + session;
  struct curl_slist * chunk = curl_slist_append (NULL, header.c_str ());
  curl_easy_setopt (curl, CURLOPT_HTTPHEADER, chunk);
  // POST?
  if (! request.empty ())
  {
    curl_easy_setopt (curl, CURLOPT_POST, 1);
    curl_easy_setopt (curl, CURLOPT_POSTFIELDS, request.c_str ());
  }
  // Perform HTTP query.
  curl_easy_perform (curl);
  // HTTP status code.
  long http = 0;
  curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http);
  // Cleanup.
  curl_easy_cleanup (curl);
  curl_slist_free_all (chunk);
  return http;
}

/* static */
enum Freebox::Quality Freebox::ParseQuality (const string & q)
{
  if (q == "auto") return AUTO;
  if (q == "hd")   return HD;
  if (q == "sd")   return SD;
  if (q == "ld")   return LD;
  if (q == "3d")   return STEREO;
  return DEFAULT;
}

/* static */
bool Freebox::HTTP (const string & custom,
                    const string & path,
                    const Document & request,
                    Document * doc, Type type) const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  string url = URL (path);
  string session = m_session_token;
  lock.Unlock ();

  cout << custom << ' ' << url << endl;

  StringBuffer buffer;
  if (! request.IsNull ())
  {
    Writer<StringBuffer> writer (buffer);
    request.Accept (writer);
    cout << buffer.GetString () << endl;
  }

  string response;
  long http = freebox_http (custom, url, buffer.GetString (), &response, session);
  if (custom != "GET") cout << response << endl;

  doc->Parse (response);

  if (doc->HasParseError ()) return false;

  if (! doc->IsObject ()) return false;

  auto s = doc->FindMember ("success");
  if (s == doc->MemberEnd ()) return false;

  bool success = s->value.GetBool ();
  if (success && type != kNullType)
  {
    auto r = doc->FindMember ("result");
    if (r == doc->MemberEnd () || r->value.GetType () != type) return false;
  }

  if (http != 200)
  {
    XBMC->QueueNotification (QUEUE_INFO, "HTTP %d", http);
    cout << "HTTP " << http << " : " << response << endl;
    return false;
  }

  return success;
}

/* static */
bool Freebox::GET (const string & path,
                   Document * doc, Type type) const
{
  return HTTP ("GET", path, Document (), doc, type);
}

/* static */
bool Freebox::POST (const string & path,
                    const Document & request,
                    Document * doc, Type type) const
{
  return HTTP ("POST", path, request, doc, type);
}

/* static */
bool Freebox::PUT (const string & path,
                   const Document & request,
                   Document * doc, Type type) const
{
  return HTTP ("PUT", path, request, doc, type);
}

/* static */
bool Freebox::DELETE (const string & path,
                      Document * doc) const
{
  return HTTP ("DELETE", path, Document (), doc, kNullType);
}

/* static */
string Freebox::Password (const string & token, const string & challenge)
{
  unsigned char password [EVP_MAX_MD_SIZE];
  unsigned int length;

  HMAC (EVP_sha1 (),
        token.c_str (), token.length (),
        (const unsigned char *) challenge.c_str (), challenge.length (),
        password, &length);

  ostringstream oss;
  oss << hex << setfill ('0');
  for (unsigned int i = 0; i < length; ++i)
    oss << setw (2) << (int) password [i];

  return oss.str ();
}

bool Freebox::StartSession ()
{
  P8PLATFORM::CLockObject lock (m_mutex);

  if (m_app_token.empty ())
  {
    string file = m_path + "app_token.txt";
    if (! filesystem::exists (file))
    {
      char hostname [HOST_NAME_MAX + 1];
      gethostname (hostname, HOST_NAME_MAX);
      cout << "StartSession: hostname: " << hostname << endl;

      Document request (kObjectType);
      request.AddMember ("app_id",      PVR_FREEBOX_APP_ID,      request.GetAllocator ());
      request.AddMember ("app_name",    PVR_FREEBOX_APP_NAME,    request.GetAllocator ());
      request.AddMember ("app_version", PVR_FREEBOX_APP_VERSION, request.GetAllocator ());
      request.AddMember ("device_name", StringRef (hostname),    request.GetAllocator ());

      Document response;
      if (! POST ("/api/v6/login/authorize", request, &response)) return false;
      m_app_token = JSON<string> (response["result"], "app_token");
      m_track_id  = JSON<int>    (response["result"], "track_id");

      ofstream ofs (file);
      ofs << m_app_token << ' ' << m_track_id;
    }
    else
    {
      ifstream ifs (file);
      ifs >> m_app_token >> m_track_id;
    }

    //cout << "app_token: " << m_app_token << endl;
    //cout << "track_id: " << m_track_id << endl;
  }

  Document login;
  if (! GET ("/api/v6/login/", &login))
    return false;

  freebox_debug (login);
  if (! login["result"]["logged_in"].GetBool ())
  {
    Document d;
    string track = to_string (m_track_id);
    string url   = "/api/v6/login/authorize/" + track;
    if (! GET (url, &d)) return false;
    string status    = JSON<string> (d["result"], "status", "unknown");
    string challenge = JSON<string> (d["result"], "challenge");
    //string salt      = JSON<string> (d["result"], "password_salt");
    //cout << status << ' ' << challenge << ' ' << salt << endl;

    if (status == "granted")
    {
      string password = Password (m_app_token, challenge);
      //cout << "password: " << password << " [" << password.length () << ']' << endl;

      Document request (kObjectType);
      request.AddMember ("app_id",   PVR_FREEBOX_APP_ID, request.GetAllocator ());
      request.AddMember ("password", password,           request.GetAllocator ());

      Document response;
      if (! POST ("/api/v6/login/session", request, &response)) return false;
      m_session_token = JSON<string> (response["result"], "session_token");

      cout << "StartSession: session_token: " << m_session_token << endl;
      return true;
    }
    else
    {
      char * notification = XBMC->GetLocalizedString (30001); // "Authorization required"
      XBMC->QueueNotification (QUEUE_INFO, notification);
      XBMC->FreeString (notification);
      return false;
    }
  }

  return false;
}

bool Freebox::CloseSession ()
{
  if (! m_session_token.empty ())
  {
    Document response;
    return POST ("/api/v6/login/logout/", Document (), &response, kNullType);
  }

  return false;
}

class Conflict
{
  public:
    string uuid;
    int major, minor; // numéro de chaîne
    int position;     // position dans le bouquet

  public:
    inline Conflict (const string & id,
                     int n1, int n2,
                     int p) :
      uuid (id),
      major (n1), minor (n2),
      position (p)
    {
    }
};

class ConflictComparator
{
  public:
    inline bool operator() (const Conflict & c1, const Conflict & c2) const
    {
      return tie (c1.major, c1.minor) < tie (c2.major, c2.minor);
    }
};

inline string StrUUIDs (const vector<Conflict> & v)
{
  string text;
  if (! v.empty ())
  {
    text += v[0].uuid;
    for (int i = 1; i < v.size (); ++i)
      text += ", " + v[i].uuid;
  }
  return '[' + text + ']';
}

inline string StrNumber (const Conflict & c, bool minor)
{
  return to_string (c.major) + (minor ? '.' + to_string (c.minor) : "");
}

inline string StrNumbers (const vector<Conflict> & v)
{
  string text;
  switch (v.size ())
  {
    case 0:
      break;

    case 1:
      text = StrNumber (v[0], false);
      break;

    default:
      text = StrNumber (v[0], true);
      for (int i = 1; i < v.size (); ++i)
        text += ", " + StrNumber (v[i], true);
      break;
  }
  return '[' + text + ']';
}

Freebox::Stream::Stream (enum Quality quality,
                         const string & url) :
  quality (quality),
  url (url)
{
}

int Freebox::Channel::Score (enum Quality q, enum Quality q0)
{
  switch (q0)
  {
    case AUTO:
      switch (q)
      {
        case AUTO: return 1000;
        case HD:   return 100;
        case SD:   return 10;
        case LD:   return 1;
        default:   return 0;
      }

    case HD:
      switch (q)
      {
        case AUTO: return 100;
        case HD:   return 1000;
        case SD:   return 10;
        case LD:   return 1;
        default:   return 0;
      }

    case SD:
      switch (q)
      {
        case AUTO: return 100;
        case HD:   return 1;
        case SD:   return 1000;
        case LD:   return 10;
        default:   return 0;
      }

    case LD:
      switch (q)
      {
        case AUTO: return 100;
        case HD:   return 1;
        case SD:   return 10;
        case LD:   return 1000;
        default:   return 0;
      }

    case STEREO:
      return (q == STEREO) ? 1000 : 0;

    default:
      return 0;
  }
}

Freebox::Channel::Channel (const string & uuid,
                           const string & name,
                           const string & logo,
                           int major, int minor,
                           const vector<Stream> & streams) :
  radio (false),
  uuid (uuid),
  name (name),
  logo (logo),
  major (major), minor (minor),
  streams (streams)
{
}

bool Freebox::Channel::IsHidden () const
{
  return streams.empty ();
}

void Freebox::Channel::GetChannel (ADDON_HANDLE handle, bool radio) const
{
  PVR_CHANNEL channel;
  memset (&channel, 0, sizeof (PVR_CHANNEL));

  channel.iUniqueId         = ChannelId (uuid);
  channel.bIsRadio          = radio;
  channel.iChannelNumber    = major;
  channel.iSubChannelNumber = minor;
  strncpy (channel.strChannelName, name.c_str (), PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy (channel.strIconPath,    logo.c_str (), PVR_ADDON_URL_STRING_LENGTH  - 1);
  channel.bIsHidden         = IsHidden ();

  PVR->TransferChannelEntry (handle, &channel);
}

PVR_ERROR Freebox::Channel::GetStreamProperties (enum Quality q, PVR_NAMED_VALUE * properties, unsigned int * count) const
{
  if (! streams.empty ())
  {
    int index = 0;
    int score = Score (streams[0].quality, q);

    for (int i = 1; i < streams.size (); ++i)
    {
      int s = Score (streams[i].quality, q);
      if (s > score)
      {
        index = i;
        score = s;
      }
    }

    strncpy (properties[0].strName,  PVR_STREAM_PROPERTY_STREAMURL,         PVR_ADDON_NAME_STRING_LENGTH - 1);
    strncpy (properties[0].strValue, streams[index].url.c_str (),           PVR_ADDON_NAME_STRING_LENGTH - 1);
    strncpy (properties[1].strName,  PVR_STREAM_PROPERTY_ISREALTIMESTREAM,  PVR_ADDON_NAME_STRING_LENGTH - 1);
    strncpy (properties[1].strValue, "true",                                PVR_ADDON_NAME_STRING_LENGTH - 1);
    *count = 2;
  }

  return PVR_ERROR_NO_ERROR;
}

Freebox::Event::Event (const Value & e, unsigned int channel, time_t date) :
  channel  (channel),
  uuid     (JSON<string> (e, "id")),
  date     (JSON<int>    (e, "date", date)),
  duration (JSON<int>    (e, "duration")),
  title    (JSON<string> (e, "title")),
  subtitle (JSON<string> (e, "sub_title")),
  season   (JSON<int>    (e, "season_number")),
  episode  (JSON<int>    (e, "episode_number")),
  picture  (JSON<string> (e, "picture")),
  category (JSON<string> (e, "category_name")),
  plot     (JSON<string> (e, "desc")),
  outline  (JSON<string> (e, "short_desc"))
{
}

bool Freebox::ProcessChannels ()
{
  m_tv_channels.clear ();

  Document channels;
  if (! GET ("/api/v6/tv/channels", &channels)) return false;

  char * notification = XBMC->GetLocalizedString (30000); // "%d channels loaded"
  XBMC->QueueNotification (QUEUE_INFO, notification, channels["result"].MemberCount ());
  XBMC->FreeString (notification);

  //Document bouquets;
  //GET ("/api/v6/tv/bouquets", &m_tv_bouquets);

  Document bouquet;
  if (! GET ("/api/v6/tv/bouquets/freeboxtv/channels", &bouquet, kArrayType)) return false;

  // Conflict list.
  typedef vector<Conflict> Conflicts;
  // Conflicts by UUID.
  map<string, Conflicts> conflicts_by_uuid;
  // Conflicts by major.
  map<int, Conflicts> conflicts_by_major;

  const Value & r = bouquet ["result"];
  for (SizeType i = 0; i < r.Size (); ++i)
  {
    string uuid  = r[i]["uuid"].GetString ();
    int    major = r[i]["number"].GetInt ();
    int    minor = r[i]["sub_number"].GetInt ();

    Conflict c (uuid, major, minor, i);

    conflicts_by_uuid [uuid] .push_back (c);
    conflicts_by_major[major].push_back (c);
  }

  static const ConflictComparator comparator;

  for (auto & [major, v1] : conflicts_by_major)
  {
    sort (v1.begin (), v1.end (), comparator);

    for (int j = 1; j < v1.size (); ++j)
    {
      Conflicts & v2 = conflicts_by_uuid [v1[j].uuid];
      v2.erase (remove_if (v2.begin (), v2.end (),
        [major] (const Conflict & c) {return c.major == major;}));
    }

    v1.erase (v1.begin () + 1, v1.end ());
  }

  for (auto & [uuid, v1] : conflicts_by_uuid)
  {
    if (! v1.empty ())
    {
      sort (v1.begin (), v1.end (), comparator);

      for (int j = 1; j < v1.size (); ++j)
      {
        Conflicts & v2 = conflicts_by_major [v1[j].major];
        v2.erase (remove_if (v2.begin (), v2.end (),
          [&uuid] (const Conflict & c) {return c.uuid == uuid;}));
      }

      v1.erase (v1.begin () + 1, v1.end ());
    }
  }

#if 0
  for (auto i : conflicts_by_major)
    cout << i.first << " : " << StrUUIDs (i.second) << endl;
#endif

#if 0
  for (auto i = conflicts_by_uuid)
    cout << i.first << " : " << StrNumbers (i.second) << endl;
#endif

  for (auto i : conflicts_by_major)
  {
    const vector<Conflict> & q = i.second;
    if (! q.empty ())
    {
      const Conflict & ch = q.front ();
      const Value  & channel = channels["result"][ch.uuid];
      const string & name    = channel["name"].GetString ();
      const string & logo    = URL (channel["logo_url"].GetString ());
      const Value  & item    = bouquet["result"][ch.position];

      vector<Stream> data;
      if (item.HasMember("available") && item["available"].GetBool ()
       && item.HasMember("streams")   && item["streams"].IsArray ())
      {
        const Value & streams = item["streams"];
        for (SizeType i = 0; i < streams.Size (); ++i)
        {
          const Value  & s = streams [i];
          const string & q = s["quality"].GetString ();
          const string & r = s["rtsp"].GetString ();
          data.emplace_back (ParseQuality (q), r);
        }
      }
      m_tv_channels.emplace (ChannelId (ch.uuid), Channel (ch.uuid, name, logo, ch.major, ch.minor, data));
    }
  }

  return true;
}

Freebox::Freebox (const string & path,
                  int quality,
                  int days,
                  bool extended,
                  int delay) :
  m_app_token (),
  m_track_id (),
  m_session_token (),
  m_path (path),
  m_server ("mafreebox.freebox.fr"),
  m_delay (delay),
  m_tv_channels (),
  m_tv_quality (Quality (quality)),
  m_epg_queries (),
  m_epg_cache (),
  m_epg_days (0),
  m_epg_last (0),
  m_epg_extended (extended),
  m_recordings (),
  m_unique_id (1),
  m_generators (),
  m_timers ()
{
  curl_global_init (CURL_GLOBAL_ALL);
  SetDays (days);
  ProcessChannels ();
  StartSession ();
  ProcessGenerators ();
  ProcessTimers ();
  ProcessRecordings ();
  CreateThread (false);
}

Freebox::~Freebox ()
{
  StopThread ();
  CloseSession ();
  curl_global_cleanup ();
}

string Freebox::GetServer () const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_server;
}

// NOT thread-safe !
string Freebox::URL (const string & query) const
{
  return "http://" + m_server + query;
}

void Freebox::SetQuality (int q)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_tv_quality = Quality (q);
}

void Freebox::SetDays (int d)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_epg_days = d != EPG_TIMEFRAME_UNLIMITED ? min (d, 7) : 7;
}

void Freebox::SetExtended (bool e)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_epg_extended = e;
}

void Freebox::SetDelay (int d)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_delay = d;
}

void Freebox::ProcessEvent (const Event & e, EPG_EVENT_STATE state)
{
#if 0
  cout << e.uuid << " : " << e.title << ' ' << e.date << '+' << e.duration << " (" << e.channel << ')' << endl;
  cout << "  " << e.category << ' ' << e.season << 'x' << e.episode << ' ' << '"' << e.subtitle << '"' << ' ' << '[' << e.picture << ']' << endl;
  cout << "  " << '"' << e.outline << '"' << ' ' << '"' << e.plot << '"' << endl;
#endif

  string picture = e.picture;
  {
    P8PLATFORM::CLockObject lock (m_mutex);
    if (! picture.empty ()) picture = URL (picture);
  }

  EPG_TAG tag;
  memset (&tag, 0, sizeof (EPG_TAG));

  tag.iUniqueBroadcastId  = BroadcastId (e.uuid);
  tag.strTitle            = PVR_FREEBOX_C_STR (e.title);
  tag.iUniqueChannelId    = e.channel;
  tag.startTime           = e.date;
  tag.endTime             = e.date + e.duration;
  tag.strPlotOutline      = PVR_FREEBOX_C_STR (e.outline);
  tag.strPlot             = PVR_FREEBOX_C_STR (e.plot);
  tag.strOriginalTitle    = NULL;
  tag.strCast             = NULL;
  tag.strDirector         = NULL;
  tag.strWriter           = NULL;
  tag.iYear               = 0;
  tag.strIMDBNumber       = NULL;
  tag.strIconPath         = PVR_FREEBOX_C_STR (picture);
#if 0 // categories mismatch! lookup table?
  tag.iGenreType          = category & 0xF0;
  tag.iGenreSubType       = category & 0x0F;
  tag.strGenreDescription = NULL;
#else
  tag.iGenreType          = EPG_GENRE_USE_STRING;
  tag.iGenreSubType       = 0;
  tag.strGenreDescription = PVR_FREEBOX_C_STR (e.category);
#endif
  tag.iParentalRating     = 0;
  tag.iStarRating         = 0;
  tag.bNotify             = false;
  tag.iSeriesNumber       = e.season;
  tag.iEpisodeNumber      = e.episode;
  tag.iEpisodePartNumber  = 0;
  tag.strEpisodeName      = PVR_FREEBOX_C_STR (e.subtitle);
  tag.iFlags              = EPG_TAG_FLAG_UNDEFINED;

  PVR->EpgEventStateChange (&tag, state);
}

void Freebox::ProcessEvent (const Value & event, unsigned int channel, time_t date, EPG_EVENT_STATE state)
{
  {
    P8PLATFORM::CLockObject lock (m_mutex);
    auto f = m_tv_channels.find (channel);
    if (f == m_tv_channels.end () || f->second.IsHidden ()) return;
  }

  Event e (event, channel, date);

  if (state == EPG_EVENT_CREATED)
  {
    P8PLATFORM::CLockObject lock (m_mutex);
    if (m_epg_extended)
    {
      string query = "/api/v6/tv/epg/programs/" + e.uuid;
      m_epg_queries.push (Query (EVENT, query, channel, date));
    }
  }

  ProcessEvent (e, state);
}

void Freebox::ProcessChannel (const Value & epg, unsigned int channel)
{
  for (auto i = epg.MemberBegin (); i != epg.MemberEnd (); ++i)
  {
    const Value & event = i->value;

    string uuid = JSON<string> (event, "id");
    time_t date = JSON<int>    (event, "date");

    static const string PREFIX = "pluri_";
    if (uuid.find (PREFIX) != 0) continue;

    string query = "/api/v6/tv/epg/programs/" + uuid;

    {
      P8PLATFORM::CLockObject lock (m_mutex);
      if (m_epg_cache.count (query) > 0) continue;
    }

    ProcessEvent (event, channel, date, EPG_EVENT_CREATED);

    {
      P8PLATFORM::CLockObject lock (m_mutex);
      m_epg_cache.insert (query);
    }
  }
}

void Freebox::ProcessFull (const Value & epg)
{
  for (auto i = epg.MemberBegin (); i != epg.MemberEnd (); ++i)
  {
    string uuid = i->name.GetString ();
    ProcessChannel (i->value, ChannelId (uuid));
  }
}

void * Freebox::Process ()
{
  while (! IsStopped ())
  {
    m_mutex.Lock ();
    int    delay = m_delay;
    int    days  = m_epg_days;
    time_t now   = time (NULL);
    time_t end   = now + days * 24 * 60 * 60;
    time_t last  = max (now, m_epg_last);
    m_mutex.Unlock ();

    for (time_t t = last - (last % 3600); t < end; t += 3600)
    {
      string epoch = to_string (t);
      string query = "/api/v6/tv/epg/by_time/" + epoch;
      {
        P8PLATFORM::CLockObject lock (m_mutex);
        m_epg_queries.push (Query (FULL, query));
        //XBMC->Log (LOG_INFO, "Queued: '%s' %d < %d", query.c_str (), t, end);
        m_epg_last = t + 3600;
      }
    }

    Query q;
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      if (! m_epg_queries.empty ())
      {
        q = m_epg_queries.front ();
        m_epg_queries.pop ();
      }
    }

    if (q.type != NONE)
    {
      //cout << q.query << " [" << delay << ']' << endl;
      XBMC->Log (LOG_INFO, "Processing: '%s'", q.query.c_str ());

      Document json;
      if (GET (q.query, &json))
      {
        switch (q.type)
        {
          case FULL    : ProcessFull    (json["result"]); break;
          case CHANNEL : ProcessChannel (json["result"], q.channel); break;
          case EVENT   : ProcessEvent   (json["result"], q.channel, q.date, EPG_EVENT_UPDATED); break;
        }
      }
    }
    else
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      m_epg_cache.clear ();
    }

    Sleep (delay * 1000);
  }

  return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// C H A N N E L S /////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

int Freebox::GetChannelsAmount ()
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_tv_channels.size ();
}

PVR_ERROR Freebox::GetChannels (ADDON_HANDLE handle, bool radio)
{
  P8PLATFORM::CLockObject lock (m_mutex);

  //for (auto i = m_tv_channels.begin (); i != m_tv_channels.end (); ++i)
  for (auto i : m_tv_channels)
    i.second.GetChannel (handle, radio);

  return PVR_ERROR_NO_ERROR;
}

int Freebox::GetChannelGroupsAmount ()
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return 0;
}

PVR_ERROR Freebox::GetChannelGroups (ADDON_HANDLE handle, bool radio)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetChannelGroupMembers (ADDON_HANDLE handle, const PVR_CHANNEL_GROUP & group)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetChannelStreamProperties (const PVR_CHANNEL * channel, PVR_NAMED_VALUE * properties, unsigned int * count)
{
  if (! channel || ! properties || ! count || *count < 2)
    return PVR_ERROR_INVALID_PARAMETERS;

  P8PLATFORM::CLockObject lock (m_mutex);
  auto f = m_tv_channels.find (channel->iUniqueId);
  if (f != m_tv_channels.end ())
    return f->second.GetStreamProperties (m_tv_quality, properties, count);

  return PVR_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
// R E C O R D I N G S /////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

Freebox::Recording::Recording (const Value & json) :
  id              (JSON<int>    (json, "id")),
  start           (JSON<int>    (json, "start")),
  end             (JSON<int>    (json, "end")),
  name            (JSON<string> (json, "name")),
  subname         (JSON<string> (json, "subname")),
  channel_uuid    (JSON<string> (json, "channel_uuid")),
  channel_name    (JSON<string> (json, "channel_name")),
//channel_quality (JSON<string> (json, "channel_quality")),
//channel_type    (JSON<string> (json, "channel_type")),
//broadcast_type  (JSON<string> (json, "broadcast_type")),
  media           (JSON<string> (json, "media")),
  path            (JSON<string> (json, "path")),
  filename        (JSON<string> (json, "filename")),
  secure          (JSON<bool>   (json, "secure"))
{
}

void Freebox::ProcessRecordings ()
{
  m_recordings.clear ();

  Document recordings;
  if (GET ("/api/v6/pvr/finished/", &recordings, kArrayType))
  {
    Value & result = recordings ["result"];
    for (SizeType i = 0; i < result.Size (); ++i)
    {
      int id = result[i]["id"].GetInt ();
      m_recordings.emplace (id, Recording (result [i]));
    }

    PVR->TriggerRecordingUpdate ();
  }
}

int Freebox::GetRecordingsAmount (bool deleted) const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_recordings.size ();
}

PVR_ERROR Freebox::GetRecordings (ADDON_HANDLE handle, bool deleted) const
{
  P8PLATFORM::CLockObject lock (m_mutex);

  for (auto & [id, r] : m_recordings)
    if (! r.secure)
    {
      PVR_RECORDING recording;
      memset (&recording, 0, sizeof (PVR_RECORDING));

      recording.recordingTime = r.start;
      recording.iDuration     = r.end - r.start;
      recording.iChannelUid   = ChannelId (r.channel_uuid);
      recording.channelType   = PVR_RECORDING_CHANNEL_TYPE_TV; // r.broadcast_type == "tv"

      strncpy (recording.strRecordingId, to_string (r.id).c_str (), PVR_ADDON_NAME_STRING_LENGTH - 1);
      strncpy (recording.strTitle,       r.name.c_str (),           PVR_ADDON_NAME_STRING_LENGTH - 1);
      strncpy (recording.strEpisodeName, r.subname.c_str (),        PVR_ADDON_NAME_STRING_LENGTH - 1);
      strncpy (recording.strChannelName, r.channel_name.c_str (),   PVR_ADDON_NAME_STRING_LENGTH - 1);

      PVR->TransferRecordingEntry (handle, &recording);
    }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetRecordingStreamProperties (const PVR_RECORDING * recording, PVR_NAMED_VALUE * properties, unsigned int * count) const
{
  if (! recording || ! properties || ! count || *count < 2)
    return PVR_ERROR_INVALID_PARAMETERS;

  int id = stoi (recording->strRecordingId);

  P8PLATFORM::CLockObject lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  const Recording & r = i->second;
  string stream = "smb://" + m_server + '/' + r.media + '/' + r.path + '/' + r.filename;
  strncpy (properties[0].strName,  PVR_STREAM_PROPERTY_STREAMURL,        PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy (properties[0].strValue, stream.c_str (),                      PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy (properties[1].strName,  PVR_STREAM_PROPERTY_ISREALTIMESTREAM, PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy (properties[1].strValue, "false",                              PVR_ADDON_NAME_STRING_LENGTH - 1);
  *count = 2;

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::RenameRecording (const PVR_RECORDING & recording)
{
  StartSession ();

  int    id      = stoi (recording.strRecordingId);
  string name    = recording.strTitle;
  string subname = recording.strEpisodeName;

  P8PLATFORM::CLockObject lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  // Payload.
  Document d (kObjectType);
  d.AddMember ("name",    name,    d.GetAllocator ());
  d.AddMember ("subname", subname, d.GetAllocator ());

  // Update recording (Freebox).
  Document response;
  if (! PUT ("/api/v6/pvr/finished/" + to_string (id), d, &response))
    return PVR_ERROR_SERVER_ERROR;

  // Update recording (locally).
  i->second = Recording (response["result"]);
  PVR->TriggerRecordingUpdate ();

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::DeleteRecording (const PVR_RECORDING & recording)
{
  StartSession ();

  int id = stoi (recording.strRecordingId);

  P8PLATFORM::CLockObject lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  // Delete recording (Freebox).
  Document response;
  if (! DELETE ("/api/v6/pvr/finished/" + to_string (id), &response))
    return PVR_ERROR_SERVER_ERROR;

  // Delete recording (locally).
  m_recordings.erase (i);
  PVR->TriggerRecordingUpdate ();

  return PVR_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
// T I M E R S /////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

Freebox::Generator::Generator (const Value & json) :
  id               (JSON<int>    (json, "id")),
//type             (JSON<string> (json, "type")),
  media            (JSON<string> (json, "media")),
  path             (JSON<string> (json, "path")),
  name             (JSON<string> (json, "name")),
//subname          (JSON<string> (json, "name")),
  start_hour       (JSON<int>    (json["params"], "start_hour")),
  start_min        (JSON<int>    (json["params"], "start_min")),
  duration         (JSON<int>    (json["params"], "duration")),
  margin_before    (JSON<int>    (json["params"], "margin_before")),
  margin_after     (JSON<int>    (json["params"], "margin_after")),
  channel_uuid     (JSON<string> (json["params"], "channel_uuid")),
//channel_quality  (JSON<string> (json["params"], "channel_quality")),
//channel_type     (JSON<string> (json["params"], "channel_type")),
//channel_strict   (JSON<bool>   (json["params"], "channel_strict"))
  repeat_monday    (JSON<bool>   (json["params"]["repeat_days"], "monday")),
  repeat_tuesday   (JSON<bool>   (json["params"]["repeat_days"], "tuesday")),
  repeat_wednesday (JSON<bool>   (json["params"]["repeat_days"], "wednesday")),
  repeat_thursday  (JSON<bool>   (json["params"]["repeat_days"], "thursday")),
  repeat_friday    (JSON<bool>   (json["params"]["repeat_days"], "friday")),
  repeat_saturday  (JSON<bool>   (json["params"]["repeat_days"], "saturday")),
  repeat_sunday    (JSON<bool>   (json["params"]["repeat_days"], "sunday"))
{
}

void Freebox::ProcessGenerators ()
{
  m_generators.clear ();

  Document generators;
  if (GET ("/api/v6/pvr/generator/", &generators, kArrayType))
  {
    Value & result = generators ["result"];
    for (SizeType i = 0; i < result.Size (); ++i)
    {
      int        id = result[i]["id"].GetInt ();
      int unique_id = m_unique_id ("generator/" + to_string (id));
      m_generators.emplace (unique_id, Generator (result [i]));
    }

    PVR->TriggerTimerUpdate ();
  }
}

Freebox::Timer::Timer (const Value & json) :
  id             (JSON<int>    (json, "id")),
  start          (JSON<int>    (json, "start")),
  end            (JSON<int>    (json, "end")),
  margin_before  (JSON<int>    (json, "margin_before")),
  margin_after   (JSON<int>    (json, "margin_after")),
  name           (JSON<string> (json, "name")),
  subname        (JSON<string> (json, "subname")),
  channel_uuid   (JSON<string> (json, "channel_uuid")),
  channel_name   (JSON<string> (json, "channel_name")),
//channel_type   (JSON<string> (json, "channel_type")),
//broadcast_type (JSON<string> (json, "broadcast_type")),
  media          (JSON<string> (json, "media")),
  path           (JSON<string> (json, "path")),
  has_record_gen (JSON<bool>   (json, "has_record_gen")),
  record_gen_id  (JSON<int>    (json, "record_gen_id")),
  enabled        (JSON<bool>   (json, "enabled")),
  conflict       (JSON<bool>   (json, "conflict")),
  state          (JSON<string> (json, "state")),
  error          (JSON<string> (json, "error"))
{
}

void Freebox::ProcessTimers ()
{
  m_timers.clear ();

  Document timers;
  if (GET ("/api/v6/pvr/programmed/", &timers, kArrayType))
  {
    Value & result = timers ["result"];
    for (SizeType i = 0; i < result.Size (); ++i)
    {
      int        id = result[i]["id"].GetInt ();
      int unique_id = m_unique_id ("programmed/" + to_string (id));
      m_timers.emplace (unique_id, Timer (result [i]));
    }

    PVR->TriggerTimerUpdate ();
  }
}

PVR_ERROR Freebox::GetTimerTypes (PVR_TIMER_TYPE types [], int * size) const
{
  if (! size)
    return PVR_ERROR_SERVER_ERROR;

  if (*size < 5)
    return PVR_ERROR_INVALID_PARAMETERS;

  const unsigned int ATTRIBS =
    PVR_TIMER_TYPE_SUPPORTS_CHANNELS         |
    PVR_TIMER_TYPE_SUPPORTS_START_TIME       |
    PVR_TIMER_TYPE_SUPPORTS_END_TIME         |
    PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN;

  // One-shot manual.
  //strncpy (types[0].strDescription, "PVR_FREEBOX_TIMER_MANUAL", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[0].iId = PVR_FREEBOX_TIMER_MANUAL;
  types[0].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_IS_MANUAL;
  types[0].iPrioritiesSize = 0;
  types[0].iLifetimesSize = 0;
  types[0].iPreventDuplicateEpisodesSize = 0;
  types[0].iRecordingGroupSize = 0;
  types[0].iMaxRecordingsSize = 0;

  // One-shot EPG.
  //strncpy (types[1].strDescription, "PVR_FREEBOX_TIMER_EPG", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[1].iId = PVR_FREEBOX_TIMER_EPG;
  types[1].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_REQUIRES_EPG_TAG_ON_CREATE;
  types[1].iPrioritiesSize = 0;
  types[1].iLifetimesSize = 0;
  types[1].iPreventDuplicateEpisodesSize = 0;
  types[1].iRecordingGroupSize = 0;
  types[1].iMaxRecordingsSize = 0;

  // One-shot generated (read-only).
  //strncpy (types[2].strDescription, "PVR_FREEBOX_TIMER_GENERATED", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[2].iId = PVR_FREEBOX_TIMER_GENERATED;
  types[2].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_IS_READONLY |
                         PVR_TIMER_TYPE_FORBIDS_NEW_INSTANCES |
                         PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE;
  types[2].iPrioritiesSize = 0;
  types[2].iLifetimesSize = 0;
  types[2].iPreventDuplicateEpisodesSize = 0;
  types[2].iRecordingGroupSize = 0;
  types[2].iMaxRecordingsSize = 0;

  // Repeating manual.
  //strncpy (types[3].strDescription, "PVR_FREEBOX_GENERATOR_MANUAL", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[3].iId = PVR_FREEBOX_GENERATOR_MANUAL;
  types[3].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_IS_MANUAL |
                         PVR_TIMER_TYPE_IS_REPEATING |
                         PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS;
  types[3].iPrioritiesSize = 0;
  types[3].iLifetimesSize = 0;
  types[3].iPreventDuplicateEpisodesSize = 0;
  types[3].iRecordingGroupSize = 0;
  types[3].iMaxRecordingsSize = 0;

  // Repeating EPG.
  //strncpy (types[4].strDescription, "PVR_FREEBOX_GENERATOR_EPG", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[4].iId = PVR_FREEBOX_GENERATOR_EPG;
  types[4].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_IS_REPEATING |
                         PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS |
                         PVR_TIMER_TYPE_REQUIRES_EPG_TAG_ON_CREATE;
  types[4].iPrioritiesSize = 0;
  types[4].iLifetimesSize = 0;
  types[4].iPreventDuplicateEpisodesSize = 0;
  types[4].iRecordingGroupSize = 0;
  types[4].iMaxRecordingsSize = 0;

  *size = 5;
  return PVR_ERROR_NO_ERROR;
}

int Freebox::GetTimersAmount () const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_generators.size () + m_timers.size ();
}

PVR_ERROR Freebox::GetTimers (ADDON_HANDLE handle) const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  //cout << "Freebox::GetTimers" << endl;

  for (auto & [id, g] : m_generators)
  {
    PVR_TIMER timer;
    memset (&timer, 0, sizeof (PVR_TIMER));

    time_t now = time (NULL);
    tm today = *localtime (&now);
    today.tm_hour = g.start_hour;
    today.tm_min  = g.start_min;
    today.tm_sec  = 0;
    time_t start = mktime (&today);

    timer.iTimerType         = PVR_FREEBOX_GENERATOR_MANUAL;
    timer.iParentClientIndex = PVR_TIMER_NO_PARENT;
    timer.iClientIndex       = id;
    timer.iClientChannelUid  = ChannelId (g.channel_uuid);
    timer.startTime          = start;
    timer.endTime            = start + g.duration;
    timer.iMarginStart       = g.margin_before / 60;
    timer.iMarginEnd         = g.margin_after  / 60;
    timer.iWeekdays          = (g.repeat_monday    ? PVR_WEEKDAY_MONDAY    : 0) |
                               (g.repeat_tuesday   ? PVR_WEEKDAY_TUESDAY   : 0) |
                               (g.repeat_wednesday ? PVR_WEEKDAY_WEDNESDAY : 0) |
                               (g.repeat_thursday  ? PVR_WEEKDAY_THURSDAY  : 0) |
                               (g.repeat_friday    ? PVR_WEEKDAY_FRIDAY    : 0) |
                               (g.repeat_saturday  ? PVR_WEEKDAY_SATURDAY  : 0) |
                               (g.repeat_sunday    ? PVR_WEEKDAY_SUNDAY    : 0);

    strncpy (timer.strTitle, g.name.c_str (), PVR_ADDON_NAME_STRING_LENGTH - 1);

    PVR->TransferTimerEntry (handle, &timer);
  }

  for (auto & [id, t] : m_timers)
  {
    PVR_TIMER timer;
    memset (&timer, 0, sizeof (PVR_TIMER));

    if (t.has_record_gen)
    {
      timer.iTimerType         = PVR_FREEBOX_TIMER_GENERATED;
      timer.iParentClientIndex = m_unique_id ("generator/" + to_string (t.record_gen_id));
    }
    else
    {
      timer.iTimerType         = PVR_FREEBOX_TIMER_MANUAL;
      timer.iParentClientIndex = PVR_TIMER_NO_PARENT;
    }

    timer.iClientIndex       = id;
    timer.iClientChannelUid  = ChannelId (t.channel_uuid);
    timer.startTime          = t.start;
    timer.endTime            = t.end;
    timer.iMarginStart       = t.margin_before / 60;
    timer.iMarginEnd         = t.margin_after  / 60;

    /**/ if (t.state == "disabled")           timer.state = PVR_TIMER_STATE_DISABLED;
    else if (t.state == "start_error")        timer.state = PVR_TIMER_STATE_ERROR;
    else if (t.state == "waiting_start_time") timer.state = t.conflict ? PVR_TIMER_STATE_CONFLICT_NOK : PVR_TIMER_STATE_SCHEDULED;
    else if (t.state == "starting")           timer.state = PVR_TIMER_STATE_RECORDING;
    else if (t.state == "running")            timer.state = PVR_TIMER_STATE_RECORDING;
    else if (t.state == "running_error")      timer.state = PVR_TIMER_STATE_ERROR;
    else if (t.state == "failed")             timer.state = PVR_TIMER_STATE_ERROR;
    else if (t.state == "finished")           timer.state = PVR_TIMER_STATE_COMPLETED;

    strncpy (timer.strTitle, t.name.c_str (), PVR_ADDON_NAME_STRING_LENGTH - 1);

    PVR->TransferTimerEntry (handle, &timer);
  }

  return PVR_ERROR_NO_ERROR;
}

inline Document freebox_generator_request (const PVR_TIMER & timer)
{
  string channel_uuid = "uuid-webtv-" + to_string (timer.iClientChannelUid);
  string title        = timer.strTitle;
  tm     date         = *localtime (&timer.startTime);
  int    duration     = timer.endTime - timer.startTime;

  Document d (kObjectType);
  Document::AllocatorType & a = d.GetAllocator ();
  d.AddMember ("type", "manual_repeat", a);
  d.AddMember ("name", title,           a);
  Value p (kObjectType);
  p.AddMember ("start_hour",    date.tm_hour, a);
  p.AddMember ("start_min",     date.tm_min,  a);
  p.AddMember ("start_sec",     0,            a);
  p.AddMember ("duration",      duration,     a);
  p.AddMember ("margin_before", timer.iMarginStart * 60, a);
  p.AddMember ("margin_after",  timer.iMarginEnd   * 60, a);
  p.AddMember ("channel_uuid",  channel_uuid, a);
  Value r (kObjectType);
  r.AddMember ("monday",      (timer.iWeekdays & PVR_WEEKDAY_MONDAY)    != 0, a);
  r.AddMember ("tuesday",     (timer.iWeekdays & PVR_WEEKDAY_TUESDAY)   != 0, a);
  r.AddMember ("wednesday",   (timer.iWeekdays & PVR_WEEKDAY_WEDNESDAY) != 0, a);
  r.AddMember ("thursday",    (timer.iWeekdays & PVR_WEEKDAY_THURSDAY)  != 0, a);
  r.AddMember ("friday",      (timer.iWeekdays & PVR_WEEKDAY_FRIDAY)    != 0, a);
  r.AddMember ("saturday",    (timer.iWeekdays & PVR_WEEKDAY_SATURDAY)  != 0, a);
  r.AddMember ("sunday",      (timer.iWeekdays & PVR_WEEKDAY_SUNDAY)    != 0, a);
  p.AddMember ("repeat_days", r, a);
  d.AddMember ("params",      p, a);
  return d;
}

PVR_ERROR Freebox::AddTimer (const PVR_TIMER & timer)
{
  StartSession ();

  int    type         = timer.iTimerType;
  int    channel      = timer.iClientChannelUid;
  string channel_uuid = "uuid-webtv-" + to_string (channel);
  string title        = timer.strTitle;

  P8PLATFORM::CLockObject lock (m_mutex);
  switch (type)
  {
    case PVR_FREEBOX_TIMER_MANUAL :
    case PVR_FREEBOX_TIMER_EPG :
    {
      //cout << "AddTimer: TIMER[" << type << ']' << endl;

      string subtitle;
      if (timer.iEpgUid != EPG_TAG_INVALID_UID)
      {
        Document epg;
        string epg_id = "pluri_" + to_string (timer.iEpgUid);
        if (GET ("/api/v6/tv/epg/programs/" + epg_id, &epg))
        {
          Event e (epg ["result"], channel, timer.startTime);
          ostringstream oss;
          if (e.season  != 0) oss << 'S' << setfill ('0') << setw (2) << e.season;
          if (e.episode != 0) oss << 'E' << setfill ('0') << setw (2) << e.episode;
          string prefix = oss.str ();
          subtitle = (prefix.empty () ? "" : prefix + " - ") + e.subtitle;
        }
      }

      Document d (kObjectType);
      Document::AllocatorType & a = d.GetAllocator ();
      d.AddMember ("start",           (int64_t) timer.startTime, a);
      d.AddMember ("end",             (int64_t) timer.endTime,   a);
      d.AddMember ("margin_before",   timer.iMarginStart * 60,   a);
      d.AddMember ("margin_after",    timer.iMarginEnd   * 60,   a);
      d.AddMember ("channel_uuid",    channel_uuid,              a);
      d.AddMember ("channel_type",    "",                        a);
      d.AddMember ("channel_quality", "auto",                    a);
      d.AddMember ("broadcast_type",  "tv",                      a);
      d.AddMember ("name",            title,                     a);
      d.AddMember ("subname",         subtitle,                  a);
    //d.AddMember ("media",           "Disque dur",              a);
    //d.AddMember ("path",            "Enregistrements",         a);

      // Add timer (Freebox).
      Document response;
      if (! POST ("/api/v6/pvr/programmed/", d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Add timer (locally).
      int id     = response["result"]["id"].GetInt ();
      int unique = m_unique_id ("programmed/" + to_string (id));
      m_timers.emplace (unique, Timer (response["result"]));
      PVR->TriggerTimerUpdate ();

      // Update recordings if timer is running.
      string state = JSON<string> (response["result"], "state");
      //cout << "AddTimer: TIMER[" << type << "]: '" << state << "'" << endl;
      if (state == "starting" || state == "running")
        ProcessRecordings (); // FIXME: doesn't work!

      break;
    }

    case PVR_FREEBOX_GENERATOR_MANUAL :
    case PVR_FREEBOX_GENERATOR_EPG :
    {
      //cout << "AddTimer: GENERATOR[" << type << ']' << endl;

      tm  date     = *localtime (&timer.startTime);
      int duration = timer.endTime - timer.startTime;

      // Payload.
      Document d = freebox_generator_request (timer);

      // Add generator (Freebox).
      Document response;
      if (! POST ("/api/v6/pvr/generator/", d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Add generator (locally).
      int id     = response["result"]["id"].GetInt ();
      int unique = m_unique_id ("generator/" + to_string (id));
      m_generators.emplace (unique, Generator (response["result"]));

      // Reload timers.
      ProcessTimers ();
      // Reload recordings.
      // FIXME: only if there's an active generated timer.
      ProcessRecordings ();

      break;
    }

    default:
    {
      //cout << "AddTimer: UNKNOWN TYPE!" << endl;
      return PVR_ERROR_SERVER_ERROR;
    }
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::UpdateTimer (const PVR_TIMER & timer)
{
  StartSession ();

  int type = timer.iTimerType;

  switch (type)
  {
    case PVR_FREEBOX_TIMER_MANUAL :
    case PVR_FREEBOX_TIMER_EPG :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_timers.find (timer.iClientIndex);
      if (i == m_timers.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "UpdateTimer: TIMER[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      string channel_uuid = "uuid-webtv-" + to_string (timer.iClientChannelUid);
      string title        = timer.strTitle;

      // Payload.
      Document d (kObjectType);
      Document::AllocatorType & a = d.GetAllocator ();
    //d.AddMember ("enabled",         enabled,                   a);
      d.AddMember ("start",           (int64_t) timer.startTime, a);
      d.AddMember ("end",             (int64_t) timer.endTime,   a);
      d.AddMember ("margin_before",   timer.iMarginStart * 60,   a);
      d.AddMember ("margin_after",    timer.iMarginEnd   * 60,   a);
      d.AddMember ("channel_uuid",    channel_uuid,              a);
    //d.AddMember ("channel_type",    "",                        a);
    //d.AddMember ("channel_quality", "auto",                    a);
      d.AddMember ("name",            title,                     a);
    //d.AddMember ("subname",         "",                        a);
    //d.AddMember ("media",           "Disque dur",              a);
    //d.AddMember ("path",            "Enregistrements",         a);

      // Update timer (Freebox).
      Document response;
      if (! PUT ("/api/v6/pvr/programmed/" + to_string (id), d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Update timer (locally).
      i->second = Timer (response["result"]);
      //cout << "UpdateTimer: TIMER[" << type << "]: '" << i->second.state << "'" << endl;
      PVR->TriggerTimerUpdate ();

      break;
    }

    case PVR_FREEBOX_TIMER_GENERATED :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_timers.find (timer.iClientIndex);
      if (i == m_timers.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "UpdateTimer: TIMER_GENERATED: " << timer.iClientIndex << " > " << id << endl;

      // Payload.
      Document d (kObjectType);
      d.AddMember ("enabled", timer.state != PVR_TIMER_STATE_DISABLED, d.GetAllocator ());

      // Update generated timer (Freebox).
      Document response;
      if (! PUT ("/api/v6/pvr/programmed/" + to_string (id), d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Update generated timer (locally).
      i->second = Timer (response["result"]);
      //cout << "UpdateTimer: TIMER_GENERATED: '" << i->second.state << "'" << endl;
      PVR->TriggerTimerUpdate ();

      break;
    }

    case PVR_FREEBOX_GENERATOR_MANUAL :
    case PVR_FREEBOX_GENERATOR_EPG :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_generators.find (timer.iClientIndex);
      if (i == m_generators.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "UpdateTimer: GENERATOR[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      // Payload.
      Document d = freebox_generator_request (timer);

      // Update generator (Freebox).
      Document response;
      if (! PUT ("/api/v6/pvr/generator/" + to_string (id), d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Update generator (locally).
      i->second = Generator (response["result"]);
      ProcessTimers ();
      ProcessRecordings ();

      break;
    }

    default:
    {
      //cout << "UpdateTimer: UNKNOWN TYPE!" << endl;
      return PVR_ERROR_SERVER_ERROR;
    }
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::DeleteTimer (const PVR_TIMER & timer, bool force)
{
  StartSession ();

  int type = timer.iTimerType;

  switch (type)
  {
    case PVR_FREEBOX_TIMER_MANUAL :
    case PVR_FREEBOX_TIMER_EPG :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_timers.find (timer.iClientIndex);
      if (i == m_timers.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "DeleteTimer: TIMER[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      // Delete timer (Freebox).
      Document response;
      if (! DELETE ("/api/v6/pvr/programmed/" + to_string (id), &response))
        return PVR_ERROR_SERVER_ERROR;

      // Delete timer (locally).
      m_timers.erase (i);
      PVR->TriggerTimerUpdate ();

      // Update recordings if timer was running.
      if (timer.state == PVR_TIMER_STATE_RECORDING)
        ProcessRecordings ();

      break;
    }

    case PVR_FREEBOX_GENERATOR_MANUAL :
    case PVR_FREEBOX_GENERATOR_EPG :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_generators.find (timer.iClientIndex);
      if (i == m_generators.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "DeleteTimer: GENERATOR[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      // Delete generator (Freebox).
      Document response;
      if (! DELETE ("/api/v6/pvr/generator/" + to_string (id), &response))
        return PVR_ERROR_SERVER_ERROR;

      // Update recordings?
      bool r = false;

      // Delete generated timers (locally).
      for (auto i = m_timers.begin (); i != m_timers.end ();)
        if (i->second.record_gen_id == id)
        {
          r = true;
          i = m_timers.erase (i);
        }
        else
          ++i;

      // Delete generator (locally).
      m_generators.erase (i);
      PVR->TriggerTimerUpdate ();

      // Update recordings.
      if (r) ProcessRecordings ();

      break;
    }

    default:
    {
      //cout << "DeleteTimer: UNKNOWN TYPE!" << endl;
      return PVR_ERROR_SERVER_ERROR;
    }
  }

  return PVR_ERROR_NO_ERROR;
}

