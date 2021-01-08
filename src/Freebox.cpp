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
#include <numeric> // accumulate

#undef major
#undef minor

#include "kodi/Filesystem.h"
#include "kodi/General.h"
#include "kodi/Network.h"
#include "kodi/gui/dialogs/Select.h"
#include "kodi/tools/StringUtils.h"

#include "Freebox.h"

#include "openssl/sha.h"
#include "openssl/hmac.h"
#include "openssl/bio.h"
#include "openssl/buffer.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/ostreamwrapper.h"

#ifdef CreateDirectory
#undef CreateDirectory
#endif // CreateDirectory

using namespace std;
using namespace rapidjson;

#define PVR_FREEBOX_TIMER_MANUAL     1
#define PVR_FREEBOX_TIMER_EPG        2
#define PVR_FREEBOX_TIMER_GENERATED  3
#define PVR_FREEBOX_GENERATOR_MANUAL 4
#define PVR_FREEBOX_GENERATOR_EPG    5

inline
void freebox_debug (const Value & data)
{
  OStreamWrapper wrapper (cout);
  Writer<OStreamWrapper> writer (wrapper);
  data.Accept (writer);
  cout << endl;
}

inline
string freebox_base64 (const char * buffer, unsigned int length)
{
  BIO * b64 = BIO_new (BIO_f_base64 ());
  BIO * mem = BIO_new (BIO_s_mem ());
  BIO * bio = BIO_push (b64, mem);

  BIO_set_flags (bio, BIO_FLAGS_BASE64_NO_NL);

  BIO_write (bio, buffer, length);
  BIO_flush (bio);

  BUF_MEM * b;
  BIO_get_mem_ptr (bio, &b);
  string r (b->data, b->length);

  BIO_free_all (bio);

  return r;
}

inline
int freebox_http (const string & custom, const string & url, const string & request, string * response, const string & session)
{
  // URL.
  kodi::vfs::CFile f;
  if (! f.CURLCreate (url))
    return -1;
  // Custom request.
  f.CURLAddOption (ADDON_CURL_OPTION_PROTOCOL, "customrequest", custom);
  // Header.
  if (! session.empty ())
    f.CURLAddOption (ADDON_CURL_OPTION_HEADER, "X-Fbx-App-Auth", session);
  // POST?
  if (! request.empty ())
  {
    string base64 = freebox_base64 (request.c_str (), request.length ());
    f.CURLAddOption (ADDON_CURL_OPTION_PROTOCOL, "postdata", base64);
  }
  // Perform HTTP query.
  if (! f.CURLOpen (ADDON_READ_NO_CACHE))
    return -1;
  // Read HTTP response.
  char buffer [1024];
  while (int size = f.Read (buffer, 1024))
    response->append (buffer, size);
  // HTTP status code.
  string header = f.GetPropertyValue (ADDON_FILE_PROPERTY_RESPONSE_PROTOCOL, "");
  istringstream iss (header); string protocol; int status;
  if (! (iss >> protocol >> status >> ws)) return -1;
  return status;
}

/* static */
enum Freebox::Source Freebox::ParseSource (const string & s)
{
  if (s == "")     return Source::AUTO;
  if (s == "iptv") return Source::IPTV;
  if (s == "dvb")  return Source::DVB;
  return Source::DEFAULT;
}

/* static */
enum Freebox::Quality Freebox::ParseQuality (const string & q)
{
  if (q == "auto") return Quality::AUTO;
  if (q == "hd")   return Quality::HD;
  if (q == "sd")   return Quality::SD;
  if (q == "ld")   return Quality::LD;
  if (q == "3d")   return Quality::STEREO;
  return Quality::DEFAULT;
}

/* static */
enum Freebox::Protocol Freebox::ParseProtocol (const string & p)
{
  if (p == "rtsp") return Protocol::RTSP;
  if (p == "hls")  return Protocol::HLS;
  return Protocol::DEFAULT;
}

/* static */
string Freebox::StrSource (enum Source s)
{
  switch (s)
  {
    case Source::AUTO : return "";
    case Source::IPTV : return "iptv";
    case Source::DVB  : return "dvb";
    default           : return "";
  }
}

/* static */
string Freebox::StrQuality (enum Quality q)
{
  switch (q)
  {
    case Quality::AUTO   : return "auto";
    case Quality::HD     : return "hd";
    case Quality::SD     : return "sd";
    case Quality::LD     : return "ld";
    case Quality::STEREO : return "3d";
    default              : return "";
  }
}

/* static */
string Freebox::StrProtocol (enum Protocol p)
{
  switch (p)
  {
    case Protocol::RTSP : return "rtsp";
    case Protocol::HLS  : return "hls";
    default             : return "";
  }
}

/* static */
bool Freebox::Http (const string & custom,
                    const string & path,
                    const Document & request,
                    Document * doc, Type type) const
{
  m_mutex.lock ();
  string url = URL (path);
  string session = m_session_token;
  m_mutex.unlock ();

  StringBuffer buffer;
  if (! request.IsNull ())
  {
    Writer<StringBuffer> writer (buffer);
    request.Accept (writer);
  }

  string response;
  long http = freebox_http (custom, url, buffer.GetString (), &response, session);
  kodi::Log (ADDON_LOG_DEBUG, "%s %s %s", custom.c_str (), url.c_str (), response.c_str ());

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
    kodi::QueueFormattedNotification (QUEUE_INFO, "HTTP %d", http);
    cout << "HTTP " << http << " : " << response << endl;
    return false;
  }

  return success;
}

/* static */
bool Freebox::HttpGet (const string & path,
                   Document * doc, Type type) const
{
  return Http ("GET", path, Document (), doc, type);
}

/* static */
bool Freebox::HttpPost (const string & path,
                    const Document & request,
                    Document * doc, Type type) const
{
  return Http ("POST", path, request, doc, type);
}

/* static */
bool Freebox::HttpPut (const string & path,
                   const Document & request,
                   Document * doc, Type type) const
{
  return Http ("PUT", path, request, doc, type);
}

/* static */
bool Freebox::HttpDelete (const string & path,
                      Document * doc) const
{
  return Http ("DELETE", path, Document (), doc, kNullType);
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
  lock_guard<recursive_mutex> lock (m_mutex);

  if (m_app_token.empty ())
  {
    string file = m_path + "app_token.txt";
    if (! kodi::vfs::FileExists (file, false))
    {
      const string hostname = kodi::network::GetHostname ();
      cout << "StartSession: hostname: " << hostname << endl;

      Document request (kObjectType);
      request.AddMember ("app_id",      PVR_FREEBOX_APP_ID,      request.GetAllocator ());
      request.AddMember ("app_name",    PVR_FREEBOX_APP_NAME,    request.GetAllocator ());
      request.AddMember ("app_version", PVR_FREEBOX_APP_VERSION, request.GetAllocator ());
      request.AddMember ("device_name", hostname,                request.GetAllocator ());

      Document response;
      if (! HttpPost ("/api/v6/login/authorize", request, &response)) return false;
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
  if (! HttpGet ("/api/v6/login/", &login))
    return false;

  if (! login["result"]["logged_in"].GetBool ())
  {
    Document d;
    string track = to_string (m_track_id);
    string url   = "/api/v6/login/authorize/" + track;
    if (! HttpGet (url, &d)) return false;
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
      if (! HttpPost ("/api/v6/login/session", request, &response)) return false;
      m_session_token = JSON<string> (response["result"], "session_token");

      cout << "StartSession: session_token: " << m_session_token << endl;
      return true;
    }
    else
    {
      string notification = kodi::GetLocalizedString (PVR_FREEBOX_STRING_AUTH_REQUIRED);
      kodi::QueueNotification (QUEUE_WARNING, "", notification);
      return false;
    }
  }

  return true;
}

bool Freebox::CloseSession ()
{
  if (! m_session_token.empty ())
  {
    Document response;
    return HttpPost ("/api/v6/login/logout/", Document (), &response, kNullType);
  }

  return true;
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
    for (size_t i = 1; i < v.size (); ++i)
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
      for (size_t i = 1; i < v.size (); ++i)
        text += ", " + StrNumber (v[i], true);
      break;
  }
  return '[' + text + ']';
}

Freebox::Stream::Stream (enum Source  source,
                         enum Quality quality,
                         const string & rtsp,
                         const string & hls) :
  source (source),
  quality (quality),
  rtsp (rtsp),
  hls (hls)
{
}

int Freebox::Stream::score (enum Source s) const
{
  switch (s)
  {
    case Source::AUTO:
      switch (source)
      {
        case Source::AUTO: return 100;
        case Source::IPTV: return 10;
        case Source::DVB:  return 1;
        default:           return 0;
      }

    case Source::IPTV:
      switch (source)
      {
        case Source::AUTO: return 10;
        case Source::IPTV: return 100;
        case Source::DVB:  return 1;
        default:           return 0;
      }

    case Source::DVB:
      switch (source)
      {
        case Source::AUTO: return 10;
        case Source::IPTV: return 1;
        case Source::DVB:  return 100;
        default:           return 0;
      }

    default:
      return 0;
  }
}

int Freebox::Stream::score (enum Quality q) const
{
  switch (q)
  {
    case Quality::AUTO:
      switch (quality)
      {
        case Quality::AUTO: return 1000;
        case Quality::HD:   return 100;
        case Quality::SD:   return 10;
        case Quality::LD:   return 1;
        default:            return 0;
      }

    case Quality::HD:
      switch (quality)
      {
        case Quality::AUTO: return 100;
        case Quality::HD:   return 1000;
        case Quality::SD:   return 10;
        case Quality::LD:   return 1;
        default:            return 0;
      }

    case Quality::SD:
      switch (quality)
      {
        case Quality::AUTO: return 100;
        case Quality::HD:   return 1;
        case Quality::SD:   return 1000;
        case Quality::LD:   return 10;
        default:            return 0;
      }

    case Quality::LD:
      switch (quality)
      {
        case Quality::AUTO: return 100;
        case Quality::HD:   return 1;
        case Quality::SD:   return 10;
        case Quality::LD:   return 1000;
        default:            return 0;
      }

    case Quality::STEREO:
      return (quality == Quality::STEREO) ? 1000 : 0;

    default:
      return 0;
  }
}

int Freebox::Stream::score (enum Source s, enum Quality q) const
{
  return 10000 * score (s) + score (q);
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

void Freebox::Channel::GetChannel (kodi::addon::PVRChannelsResultSet & results, bool radio) const
{
  kodi::addon::PVRChannel channel;

  channel.SetUniqueId         (ChannelId (uuid));
  channel.SetIsRadio          (radio);
  channel.SetChannelNumber    (major);
  channel.SetSubChannelNumber (minor);
  channel.SetChannelName      (name);
  channel.SetIconPath         (logo);
  channel.SetIsHidden         (IsHidden ());

  results.Add (channel);
}

void freebox_debug_stream_properties (const string & url, int index, int score)
{
  kodi::Log (ADDON_LOG_DEBUG, "GetStreamProperties: '%s' (index = %d, score = %d)", url.c_str (), index, score);
}

PVR_ERROR Freebox::Channel::GetStreamProperties (enum Source source,
                                                 enum Quality quality,
                                                 enum Protocol protocol,
                                                 std::vector<kodi::addon::PVRStreamProperty> & properties) const
{
  if (! streams.empty ())
  {
    int index = 0;
    int score = streams[0].score (source, quality);
    freebox_debug_stream_properties (streams[0].rtsp, index, score);

    for (size_t i = 1; i < streams.size (); ++i)
    {
      int s = streams[i].score (source, quality);
      freebox_debug_stream_properties (streams[i].rtsp, i, s);
      if (s > score)
      {
        index = i;
        score = s;
      }
    }

    switch (protocol)
    {
      case Protocol::RTSP : properties.emplace_back (PVR_STREAM_PROPERTY_STREAMURL, streams[index].rtsp); break;
      case Protocol::HLS  : properties.emplace_back (PVR_STREAM_PROPERTY_STREAMURL, streams[index].hls);  break;
    }

    properties.emplace_back (PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");
  }

  return PVR_ERROR_NO_ERROR;
}

string Freebox::Event::Native (int c)
{
  switch (c)
  {
    case  1: return "Film";
    case  2: return "Téléfilm";
    case  3: return "Série/Feuilleton";
    case  4: return "Feuilleton";
    case  5: return "Documentaire";
    case  6: return "Théâtre";
    case  7: return "Opéra";
    case  8: return "Ballet";
    case  9: return "Variétés";
    case 10: return "Magazine";
    case 11: return "Jeunesse";
    case 12: return "Jeu";
    case 13: return "Musique";
    case 14: return "Divertissement";
    case 16: return "Dessin animé";
    case 19: return "Sport";
    case 20: return "Journal";
    case 22: return "Débat";
    case 24: return "Spectacle";
    case 31: return "Emission religieuse";
    default: return "";
  };
}

int Freebox::Event::Colors (int c)
{
  switch (c)
  {
    case  1: return 0x10; // Film
    case  2: return 0x10; // Téléfilm
    case  3: return 0x10; // Série/Feuilleton
    case  4: return 0x15; // Feuilleton
    case  5: return 0x23; // Documentaire
    case  6: return 0x70; // Théâtre
    case  7: return 0x65; // Opéra
    case  8: return 0x66; // Ballet
    case  9: return 0x32; // Variétés
    case 10: return 0x81; // Magazine
    case 11: return 0x50; // Jeunesse
    case 12: return 0x31; // Jeu
    case 13: return 0x60; // Musique
    case 14: return 0x32; // Divertissement
    case 16: return 0x55; // Dessin animé
    case 19: return 0x40; // Sport
    case 20: return 0x21; // Journal
    case 22: return 0x24; // Débat
    case 24: return 0x70; // Spectacle
    case 31: return 0x73; // Emission religieuse
    default: return 0x00;
  };
}

Freebox::Event::CastMember::CastMember (const Value & c) :
  job        (JSON<string> (c, "job")),
  first_name (JSON<string> (c, "first_name")),
  last_name  (JSON<string> (c, "last_name")),
  role       (JSON<string> (c, "role"))
{
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
  category (JSON<int>    (e, "category")),
  picture  (JSON<string> (e, "picture_big", JSON<string> (e, "picture"))),
  plot     (JSON<string> (e, "desc")),
  outline  (JSON<string> (e, "short_desc")),
  year     (JSON<int>    (e, "year")),
  cast     ()
{
  if (category != 0 && Colors (category) == 0)
  {
    string name = JSON<string> (e, "category_name");
    cout << category << " : " << name << endl;
  }

  auto f = e.FindMember ("cast");
  if (f != e.MemberEnd ())
  {
    const Value & c = f->value;
    if (c.IsArray ())
      for (SizeType i = 0; i < c.Size (); ++i)
        cast.emplace_back (c[i]);
  }
}

Freebox::Event::ConcatIfJob::ConcatIfJob (const string & job) :
  m_job (job)
{
}

string Freebox::Event::ConcatIfJob::operator() (const string & input, const Freebox::Event::CastMember & m) const
{
  if (m.job != m_job) return input;
  return (input.empty () ? "" : input + EPG_STRING_TOKEN_SEPARATOR) + (m.first_name + ' ' + m.last_name);
}

string Freebox::Event::GetCastDirector () const
{
  static const ConcatIfJob CONCAT ("Réalisateur");
  return accumulate (cast.begin (), cast.end (), string (), CONCAT);
}

string Freebox::Event::GetCastActors () const
{
  static const ConcatIfJob CONCAT ("Acteur");
  return accumulate (cast.begin (), cast.end (), string (), CONCAT);
}

inline string freebox_replace_server (string url, const string & server)
{
  static const string SERVER = "mafreebox.freebox.fr";
  size_t k = url.find (SERVER);
  return k != string::npos ? url.replace (k, SERVER.length (), server) : url;
}

void freebox_channel_logo_fix (const std::string & url, const std::string & path)
{
  string response;
  long http = freebox_http ("GET", url, "", &response, "");
  std::cout << url << " : " << http << " (" << response.length () << ')' << std::endl;
  ofstream ofs (path, ios::binary | ios::out);
  ofs.write (response.c_str (), response.length ());
  ofs.close ();
}

bool Freebox::ProcessChannels ()
{
  m_tv_channels.clear ();

  Document channels;
  if (! HttpGet ("/api/v6/tv/channels", &channels)) return false;

  string notification = kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNELS_LOADED);
  kodi::QueueFormattedNotification (QUEUE_INFO, notification.c_str (), channels["result"].MemberCount ());

  //Document bouquets;
  //HttpGet ("/api/v6/tv/bouquets", &m_tv_bouquets);

  Document bouquet;
  if (! HttpGet ("/api/v6/tv/bouquets/freeboxtv/channels", &bouquet, kArrayType)) return false;

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

#if __cplusplus >= 201703L
  for (auto & [major, v1] : conflicts_by_major)
#else
  for (auto & it : conflicts_by_major)
#endif
  {
#if __cplusplus < 201703L
    int      major = it.first;
    Conflicts & v1 = it.second;
#endif

    sort (v1.begin (), v1.end (), comparator);

    for (size_t j = 1; j < v1.size (); ++j)
    {
      Conflicts & v2 = conflicts_by_uuid [v1[j].uuid];
      v2.erase (remove_if (v2.begin (), v2.end (),
        [m = major] (const Conflict & c) {return c.major == m;}));
    }

    v1.erase (v1.begin () + 1, v1.end ());
  }

#if __cplusplus >= 201703L
  for (auto & [uuid, v1] : conflicts_by_uuid)
#else
  for (auto & it : conflicts_by_uuid)
#endif
  {
#if __cplusplus < 201703L
    const string & uuid = it.first;
    Conflicts    & v1   = it.second;
#endif

    if (! v1.empty ())
    {
      sort (v1.begin (), v1.end (), comparator);

      for (size_t j = 1; j < v1.size (); ++j)
      {
        Conflicts & v2 = conflicts_by_major [v1[j].major];
        v2.erase (remove_if (v2.begin (), v2.end (),
          [u = uuid] (const Conflict & c) {return c.uuid == u;}));
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

#if __cplusplus >= 201703L
  for (auto & [major, q] : conflicts_by_major)
#else
  for (auto & it : conflicts_by_major)
#endif
  {
#if __cplusplus < 201703L
    int           major = it.first;
    const Conflicts & q = it.second;
#endif

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
          data.emplace_back (ParseSource (s["type"].GetString ()),
                             ParseQuality (s["quality"].GetString ()),
                             freebox_replace_server (s["rtsp"].GetString (), m_server),
                             freebox_replace_server (s["hls"].GetString (), m_server));
        }
      }
#if 0
      if (! kodi::vfs::DirectoryExists (m_path + "logos"))
        kodi::vfs::CreateDirectory (m_path + "logos");

      std::string path = m_path + "logos/" + ch.uuid;
      freebox_channel_logo_fix (logo, path);
      m_tv_channels.emplace (ChannelId (ch.uuid), Channel (ch.uuid, name, path, ch.major, ch.minor, data));
#else
      m_tv_channels.emplace (ChannelId (ch.uuid), Channel (ch.uuid, name, logo + "|customrequest=GET", ch.major, ch.minor, data));
#endif
    }
  }

  {
    Document d;
    ifstream ifs (m_path + "source.txt");
    IStreamWrapper wrapper (ifs);
    d.ParseStream (wrapper);
    if (! d.HasParseError () && d.IsObject ())
      for (auto i = d.MemberBegin (); i != d.MemberEnd (); ++i)
        m_tv_prefs_source.emplace (ChannelId (i->name.GetString ()), ParseSource (i->value.GetString ()));
  }

  {
    Document d;
    ifstream ifs (m_path + "quality.txt");
    IStreamWrapper wrapper (ifs);
    d.ParseStream (wrapper);
    if (! d.HasParseError () && d.IsObject ())
      for (auto i = d.MemberBegin (); i != d.MemberEnd (); ++i)
        m_tv_prefs_quality.emplace (ChannelId (i->name.GetString ()), ParseQuality (i->value.GetString ()));
  }

  return true;
}

Freebox::Freebox () :
  m_app_token (),
  m_track_id (),
  m_session_token (),
  m_tv_channels (),
  m_tv_prefs_source (),
  m_tv_prefs_quality (),
  m_epg_queries (),
  m_epg_cache (),
  m_epg_days (0),
  m_epg_last (0),
  m_recordings (),
  m_unique_id (1),
  m_generators (),
  m_timers ()
{
}

Freebox::~Freebox ()
{
  StopThread ();
  CloseSession ();
}

void Freebox::SetServer (const string & server)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_server = server;
}

string Freebox::GetServer () const
{
  lock_guard<recursive_mutex> lock (m_mutex);
  return m_server;
}

// NOT thread-safe !
string Freebox::URL (const string & query) const
{
  return "http://" + m_server + query;
}

void Freebox::SetSource (Source s)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_tv_source = s;
}

void Freebox::SetQuality (Quality q)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_tv_quality = q;
}

void Freebox::SetProtocol (Protocol p)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_tv_protocol = p;
}

void Freebox::SetDays (int d)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_epg_days = d != EPG_TIMEFRAME_UNLIMITED ? min (d, 7) : 7;
}

void Freebox::SetExtended (bool e)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_epg_extended = e;
}

void Freebox::SetColors (bool c)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_epg_colors = c;
}

void Freebox::SetDelay (int d)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_delay = d;
}

void Freebox::ProcessEvent (const Event & e, EPG_EVENT_STATE state)
{
  // FIXME: SHOULDN'T HAPPEN!
  if (e.uuid.find ("pluri_") != 0)
  {
#if 1
  cout << e.uuid << " : " << '"' << e.title << '"' << ' ' << e.date << '+' << e.duration << " (" << e.channel << ')' << endl;
  cout << "  " << e.category << ' ' << e.season << 'x' << e.episode << ' ' << '"' << e.subtitle << '"' << ' ' << '[' << e.picture << ']' << endl;
  cout << "  " << '"' << e.outline << '"' << endl;
  cout << "  " << '"' << e.plot << '"' << endl;
#endif

    kodi::Log (ADDON_LOG_ERROR, "%s : \"%s\" %d+%d", e.uuid.c_str (), e.title.c_str (), e.date, e.duration);
    return;
  }

  m_mutex.lock ();
  bool colors = m_epg_colors;
  string picture = ! e.picture.empty () ? URL (e.picture + "|customrequest=GET") : "";
  m_mutex.unlock ();

  string actors   = e.GetCastActors   ();
  string director = e.GetCastDirector ();

  kodi::addon::PVREPGTag tag;

  tag.SetUniqueBroadcastId (BroadcastId (e.uuid));
  tag.SetTitle             (e.title);
  tag.SetUniqueChannelId   (e.channel);
  tag.SetStartTime         (e.date);
  tag.SetEndTime           (e.date + e.duration);
  tag.SetPlotOutline       (e.outline);
  tag.SetPlot              (e.plot);
  tag.SetOriginalTitle     ("");
  tag.SetCast              (actors);
  tag.SetDirector          (director);
  tag.SetWriter            ("");
  tag.SetYear              (e.year);
  tag.SetIMDBNumber        ("");
  tag.SetIconPath          (picture);
  if (colors)
  {
    int c = Event::Colors (e.category);
    tag.SetGenreType         (c & 0xF0);
    tag.SetGenreSubType      (c & 0x0F);
    tag.SetGenreDescription  ("");
  }
  else
  {
    string c = Event::Native (e.category);
    tag.SetGenreType         (EPG_GENRE_USE_STRING);
    tag.SetGenreSubType      (0);
    tag.SetGenreDescription  (c);
  }
  tag.SetParentalRating    (0);
  tag.SetStarRating        (0);
  if (e.season == 0 && e.episode == 0)
  {
    tag.SetSeriesNumber      (EPG_TAG_INVALID_SERIES_EPISODE);
    tag.SetEpisodeNumber     (EPG_TAG_INVALID_SERIES_EPISODE);
  }
  else
  {
    tag.SetSeriesNumber      (e.season);
    tag.SetEpisodeNumber     (e.episode);
  }
  tag.SetEpisodePartNumber (EPG_TAG_INVALID_SERIES_EPISODE);
  tag.SetEpisodeName       (e.subtitle);
  tag.SetFlags             (EPG_TAG_FLAG_UNDEFINED);

  EpgEventStateChange (tag, state);
}

void Freebox::ProcessEvent (const Value & event, unsigned int channel, time_t date, EPG_EVENT_STATE state)
{
  {
    lock_guard<recursive_mutex> lock (m_mutex);
    auto f = m_tv_channels.find (channel);
    if (f == m_tv_channels.end () || f->second.IsHidden ()) return;
  }

  Event e (event, channel, date);

  if (state == EPG_EVENT_CREATED)
  {
    lock_guard<recursive_mutex> lock (m_mutex);
    if (m_epg_extended)
    {
      string query = "/api/v6/tv/epg/programs/" + e.uuid;
      m_epg_queries.emplace (EVENT, query, channel, date);
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
      lock_guard<recursive_mutex> lock (m_mutex);
      if (m_epg_cache.count (query) > 0) continue;
    }

    ProcessEvent (event, channel, date, EPG_EVENT_CREATED);

    {
      lock_guard<recursive_mutex> lock (m_mutex);
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

void Freebox::Process ()
{
  while (! m_threadStop )
  {
    m_mutex.lock ();
    int    delay = m_delay;
    int    days  = m_epg_days;
    time_t now   = time (NULL);
    time_t end   = now + days * 24 * 60 * 60;
    time_t last  = max (now, m_epg_last);
    m_mutex.unlock ();

    if (StartSession ())
    {
      lock_guard<recursive_mutex> lock (m_mutex);
      ProcessGenerators ();
      ProcessTimers ();
      ProcessRecordings ();
    }

    for (time_t t = last - (last % 3600); t < end; t += 3600)
    {
      string epoch = to_string (t);
      string query = "/api/v6/tv/epg/by_time/" + epoch;
      {
        lock_guard<recursive_mutex> lock (m_mutex);
        m_epg_queries.emplace (FULL, query);
        //kodi::Log (ADDON_LOG_INFO, "Queued: '%s' %d < %d", query.c_str (), t, end);
        m_epg_last = t + 3600;
      }
    }

    Query q;
    {
      lock_guard<recursive_mutex> lock (m_mutex);
      if (! m_epg_queries.empty ())
      {
        q = m_epg_queries.front ();
        m_epg_queries.pop ();
      }
    }

    if (q.type != NONE)
    {
      //cout << q.query << " [" << delay << ']' << endl;
      kodi::Log (ADDON_LOG_INFO, "Processing: '%s'", q.query.c_str ());

      Document json;
      if (HttpGet (q.query, &json))
      {
        switch (q.type)
        {
          case FULL    : ProcessFull    (json["result"]); break;
          case CHANNEL : ProcessChannel (json["result"], q.channel); break;
          case EVENT   : ProcessEvent   (json["result"], q.channel, q.date, EPG_EVENT_UPDATED); break;
          default      : break;
        }
      }
    }
    else
    {
      lock_guard<recursive_mutex> lock (m_mutex);
      m_epg_cache.clear ();
    }

    Sleep (delay * 1000);
  }
}

////////////////////////////////////////////////////////////////////////////////
// A D D O N  - B A S I C S ////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

ADDON_STATUS Freebox::Create ()
{
  kodi::Log (ADDON_LOG_DEBUG, "%s - Creating the Freebox TV add-on", __FUNCTION__);

  m_path = UserPath ();
  if (! kodi::vfs::DirectoryExists (m_path))
    kodi::vfs::CreateDirectory (m_path);

  ReadSettings ();

  static std::vector<kodi::addon::PVRMenuhook> HOOKS =
  {
    {PVR_FREEBOX_MENUHOOK_CHANNEL_SOURCE,  PVR_FREEBOX_STRING_CHANNEL_SOURCE,  PVR_MENUHOOK_CHANNEL},
    {PVR_FREEBOX_MENUHOOK_CHANNEL_QUALITY, PVR_FREEBOX_STRING_CHANNEL_QUALITY, PVR_MENUHOOK_CHANNEL}
  };

  for (auto & h : HOOKS)
    AddMenuHook (h);

  kodi::QueueNotification (QUEUE_INFO, "", PVR_FREEBOX_VERSION);
  SetDays (EpgMaxFutureDays ());
  ProcessChannels ();
  CreateThread ();

  return ADDON_STATUS_OK;
}

ADDON_STATUS Freebox::SetSetting (const string & settingName, const kodi::CSettingValue & settingValue)
{
  /**/ if (settingName == "server")
  {
    SetServer (settingValue.GetString ());
    return ADDON_STATUS_NEED_RESTART;
  }

  else if (settingName == "delay")
    SetDelay (settingValue.GetInt ());

  else if (settingName == "restart")
    return settingValue.GetBoolean() ? ADDON_STATUS_NEED_RESTART : ADDON_STATUS_OK;

  else if (settingName == "source")
    SetSource (settingValue.GetEnum<Source> ());

  else if (settingName == "quality")
    SetQuality (settingValue.GetEnum<Quality> ());

  else if (settingName == "protocol")
    SetProtocol (settingValue.GetEnum<Protocol> ());

  else if (settingName == "extended")
    SetExtended (settingValue.GetBoolean ());

  else if (settingName == "colors")
  {
    SetColors (settingValue.GetBoolean ());
    return ADDON_STATUS_NEED_RESTART;
  }

  return ADDON_STATUS_OK;
}

void Freebox::ReadSettings ()
{
  m_server       = kodi::GetSettingString         ("server",   PVR_FREEBOX_DEFAULT_SERVER);
  m_delay        = kodi::GetSettingInt            ("delay",    PVR_FREEBOX_DEFAULT_DELAY);
  m_tv_source    = kodi::GetSettingEnum<Source>   ("source",   PVR_FREEBOX_DEFAULT_SOURCE);
  m_tv_quality   = kodi::GetSettingEnum<Quality>  ("quality",  PVR_FREEBOX_DEFAULT_QUALITY);
  m_tv_protocol  = kodi::GetSettingEnum<Protocol> ("protocol", PVR_FREEBOX_DEFAULT_PROTOCOL);
  m_epg_extended = kodi::GetSettingBoolean        ("extended", PVR_FREEBOX_DEFAULT_EXTENDED);
  m_epg_colors   = kodi::GetSettingBoolean        ("colors",   PVR_FREEBOX_DEFAULT_COLORS);
}

////////////////////////////////////////////////////////////////////////////////
// P V R  - B A S I C S ////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

PVR_ERROR Freebox::GetCapabilities (kodi::addon::PVRCapabilities & caps)
{
  caps.SetSupportsEPG                      (true);
  caps.SetSupportsTV                       (true);
  caps.SetSupportsRadio                    (false);
  caps.SetSupportsChannelGroups            (false);
  caps.SetSupportsRecordings               (true);
  caps.SetSupportsRecordingSize            (true);
  caps.SetSupportsRecordingsRename         (true);
  caps.SetSupportsRecordingsUndelete       (false);
  caps.SetSupportsRecordingsLifetimeChange (false);
  caps.SetSupportsTimers                   (true);
  caps.SetSupportsDescrambleInfo           (false);
  caps.SetSupportsAsyncEPGTransfer         (true);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetBackendName (string & name)
{
  name = PVR_FREEBOX_BACKEND_NAME;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetBackendVersion (string & version)
{
  version = PVR_FREEBOX_BACKEND_VERSION;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetBackendHostname (string & hostname)
{
  hostname = GetServer ();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetConnectionString (string & connection)
{
  connection = PVR_FREEBOX_CONNECTION_STRING;
  return PVR_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
// E P G ///////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

PVR_ERROR Freebox::SetEPGMaxFutureDays (int futureDays)
{
  SetDays (futureDays);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetEPGForChannel (int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet & results)
{
  return PVR_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
// C H A N N E L S /////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

PVR_ERROR Freebox::GetChannelsAmount (int & amount)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  amount = m_tv_channels.size ();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetChannels (bool radio, kodi::addon::PVRChannelsResultSet & results)
{
  lock_guard<recursive_mutex> lock (m_mutex);

  //for (auto i = m_tv_channels.begin (); i != m_tv_channels.end (); ++i)
  for (auto i : m_tv_channels)
    i.second.GetChannel (results, radio);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetChannelGroupsAmount (int & amount)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  amount = 0;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetChannelGroups (bool radio, kodi::addon::PVRChannelGroupsResultSet & results)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetChannelGroupMembers (const kodi::addon::PVRChannelGroup & group, kodi::addon::PVRChannelGroupMembersResultSet & results)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetChannelStreamProperties (const kodi::addon::PVRChannel & channel, std::vector<kodi::addon::PVRStreamProperty> & properties)
{
  enum Source  source  = ChannelSource  (channel.GetUniqueId (), true);
  enum Quality quality = ChannelQuality (channel.GetUniqueId (), true);

  lock_guard<recursive_mutex> lock (m_mutex);
  auto f = m_tv_channels.find (channel.GetUniqueId ());
  if (f != m_tv_channels.end ())
    return f->second.GetStreamProperties (source, quality, m_tv_protocol, properties);

  return PVR_ERROR_NO_ERROR;
}

enum Freebox::Source Freebox::ChannelSource (unsigned int id, bool fallback)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  auto f = m_tv_prefs_source.find (id);
  return f != m_tv_prefs_source.end () ? f->second : (fallback ? m_tv_source : Source::DEFAULT);
}

void Freebox::SetChannelSource (unsigned int id, enum Source source)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  switch (source)
  {
    case Source::AUTO : m_tv_prefs_source.erase (id); break;
    case Source::IPTV : m_tv_prefs_source [id] = Source::IPTV; break;
    case Source::DVB  : m_tv_prefs_source [id] = Source::DVB;  break;
    default           : break;
  }

  Document d (kObjectType);
  auto & a = d.GetAllocator ();
  for (auto & i : m_tv_prefs_source)
    d.AddMember (Value ("uuid-webtv-" + to_string (i.first), a), Value (StrSource (i.second), a), a);

  ofstream ofs (m_path + "source.txt");
  OStreamWrapper wrapper (ofs);
  Writer<OStreamWrapper> writer (wrapper);
  d.Accept (writer);
}

enum Freebox::Quality Freebox::ChannelQuality (unsigned int id, bool fallback)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  auto f = m_tv_prefs_quality.find (id);
  return f != m_tv_prefs_quality.end () ? f->second : (fallback ? m_tv_quality : Quality::DEFAULT);
}

void Freebox::SetChannelQuality (unsigned int id, enum Quality quality)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  switch (quality)
  {
    case Quality::AUTO   : m_tv_prefs_quality.erase (id); break;
    case Quality::HD     : m_tv_prefs_quality [id] = Quality::HD;     break;
    case Quality::SD     : m_tv_prefs_quality [id] = Quality::SD;     break;
    case Quality::LD     : m_tv_prefs_quality [id] = Quality::LD;     break;
    case Quality::STEREO : m_tv_prefs_quality [id] = Quality::STEREO; break;
    default              : break;
  }

  Document d (kObjectType);
  auto & a = d.GetAllocator ();
  for (auto & i : m_tv_prefs_quality)
    d.AddMember (Value ("uuid-webtv-" + to_string (i.first), a), Value (StrQuality (i.second), a), a);

  ofstream ofs (m_path + "quality.txt");
  OStreamWrapper wrapper (ofs);
  Writer<OStreamWrapper> writer (wrapper);
  d.Accept (writer);
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
  byte_size       (JSON<int>    (json, "byte_size")),
  secure          (JSON<bool>   (json, "secure"))
{
}

void Freebox::ProcessRecordings ()
{
  m_recordings.clear ();

  Document recordings;
  if (HttpGet ("/api/v6/pvr/finished/", &recordings, kArrayType))
  {
    Value & result = recordings ["result"];
    for (SizeType i = 0; i < result.Size (); ++i)
    {
      int id = result[i]["id"].GetInt ();
      m_recordings.emplace (id, Recording (result [i]));
    }

    TriggerRecordingUpdate ();
  }
}

PVR_ERROR Freebox::GetRecordingsAmount (bool deleted, int& amount)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  amount = m_recordings.size ();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetRecordings (bool deleted, kodi::addon::PVRRecordingsResultSet & results)
{
  lock_guard<recursive_mutex> lock (m_mutex);

#if __cplusplus >= 201703L
  for (auto & [id, r] : m_recordings)
#else
  for (auto & it : m_recordings)
#endif
  {
#if __cplusplus < 201703L
    const Recording & r = it.second;
#endif

    if (! r.secure)
    {
      kodi::addon::PVRRecording recording;

      recording.SetRecordingTime (r.start);
      recording.SetDuration      (r.end - r.start);
      recording.SetChannelUid    (ChannelId (r.channel_uuid));
      recording.SetChannelType   (PVR_RECORDING_CHANNEL_TYPE_TV); // r.broadcast_type == "tv"
      recording.SetRecordingId   (to_string (r.id));
      recording.SetTitle         (r.name);
      recording.SetEpisodeName   (r.subname);
      recording.SetChannelName   (r.channel_name);

      results.Add (recording);
    }
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetRecordingSize (const kodi::addon::PVRRecording & recording, int64_t & size)
{
  int id = stoi (recording.GetRecordingId ());

  lock_guard<recursive_mutex> lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  size = i->second.byte_size;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetRecordingStreamProperties (const kodi::addon::PVRRecording & recording, std::vector<kodi::addon::PVRStreamProperty> & properties)
{
  int id = stoi (recording.GetRecordingId ());

  lock_guard<recursive_mutex> lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  const Recording & r = i->second;
  string stream = "smb://" + m_server + '/' + r.media + '/' + r.path + '/' + r.filename;
  properties.emplace_back (PVR_STREAM_PROPERTY_STREAMURL, stream);
  properties.emplace_back (PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "false");

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::RenameRecording (const kodi::addon::PVRRecording & recording)
{
  StartSession ();

  int    id      = stoi (recording.GetRecordingId ());
  string name    = recording.GetTitle ();
  string subname = recording.GetEpisodeName ();

  lock_guard<recursive_mutex> lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  // Payload.
  Document d (kObjectType);
  d.AddMember ("name",    name,    d.GetAllocator ());
  d.AddMember ("subname", subname, d.GetAllocator ());

  // Update recording (Freebox).
  Document response;
  if (! HttpPut ("/api/v6/pvr/finished/" + to_string (id), d, &response))
    return PVR_ERROR_SERVER_ERROR;

  // Update recording (locally).
  i->second = Recording (response["result"]);
  TriggerRecordingUpdate ();

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::DeleteRecording (const kodi::addon::PVRRecording & recording)
{
  StartSession ();

  int id = stoi (recording.GetRecordingId ());

  lock_guard<recursive_mutex> lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  // Delete recording (Freebox).
  Document response;
  if (! HttpDelete ("/api/v6/pvr/finished/" + to_string (id), &response))
    return PVR_ERROR_SERVER_ERROR;

  // Delete recording (locally).
  m_recordings.erase (i);
  TriggerRecordingUpdate ();

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
  channel_uuid     (JSON<string> (json["params"], "channel_uuid")),
//channel_type     (JSON<string> (json["params"], "channel_type")),
//channel_quality  (JSON<string> (json["params"], "channel_quality")),
//channel_strict   (JSON<bool>   (json["params"], "channel_strict"))
//broadcast_type   (JSON<bool>   (json["params"], "broadcast_type"))
  start_hour       (JSON<int>    (json["params"], "start_hour")),
  start_min        (JSON<int>    (json["params"], "start_min")),
  duration         (JSON<int>    (json["params"], "duration")),
  margin_before    (JSON<int>    (json["params"], "margin_before")),
  margin_after     (JSON<int>    (json["params"], "margin_after")),
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
  if (HttpGet ("/api/v6/pvr/generator/", &generators, kArrayType))
  {
    Value & result = generators ["result"];
    for (SizeType i = 0; i < result.Size (); ++i)
    {
      int        id = result[i]["id"].GetInt ();
      int unique_id = m_unique_id ("generator/" + to_string (id));
      m_generators.emplace (unique_id, Generator (result [i]));
    }

    TriggerTimerUpdate ();
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
  if (HttpGet ("/api/v6/pvr/programmed/", &timers, kArrayType))
  {
    Value & result = timers ["result"];
    for (SizeType i = 0; i < result.Size (); ++i)
    {
      int        id = result[i]["id"].GetInt ();
      int unique_id = m_unique_id ("programmed/" + to_string (id));

      const string & state = result[i]["state"].GetString ();
      if (state != "finished" && state != "failed" && state != "start_error" && state != "running_error")
        m_timers.emplace (unique_id, Timer (result [i]));
    }

    TriggerTimerUpdate ();
  }
}

PVR_ERROR Freebox::GetTimerTypes (std::vector<kodi::addon::PVRTimerType> & types)
{
  const unsigned int ATTRIBS =
    PVR_TIMER_TYPE_SUPPORTS_CHANNELS         |
    PVR_TIMER_TYPE_SUPPORTS_START_TIME       |
    PVR_TIMER_TYPE_SUPPORTS_END_TIME         |
    PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN;

  // One-shot manual.
  {
    kodi::addon::PVRTimerType type;
    //type.SetDescription ("PVR_FREEBOX_TIMER_MANUAL");
    type.SetId (PVR_FREEBOX_TIMER_MANUAL);
    type.SetAttributes ( ATTRIBS |
                         PVR_TIMER_TYPE_IS_MANUAL);
    types.emplace_back (type);
  }

  // One-shot EPG.
  {
    kodi::addon::PVRTimerType type;
    //type.SetDescription ("PVR_FREEBOX_TIMER_EPG");
    type.SetId (PVR_FREEBOX_TIMER_EPG);
    type.SetAttributes ( ATTRIBS |
                         PVR_TIMER_TYPE_REQUIRES_EPG_TAG_ON_CREATE);
    types.emplace_back (type);
  }

  // One-shot generated (read-only).
  {
    kodi::addon::PVRTimerType type;
    //type.SetDescription ("PVR_FREEBOX_TIMER_GENERATED");
    type.SetId (PVR_FREEBOX_TIMER_GENERATED);
    type.SetAttributes ( ATTRIBS |
                         PVR_TIMER_TYPE_IS_READONLY |
                         PVR_TIMER_TYPE_FORBIDS_NEW_INSTANCES |
                         PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE);
    types.emplace_back (type);
  }

  // Repeating manual.
  {
    kodi::addon::PVRTimerType type;
    //type.SetDescription ("PVR_FREEBOX_GENERATOR_MANUAL");
    type.SetId (PVR_FREEBOX_GENERATOR_MANUAL);
    type.SetAttributes ( ATTRIBS |
                         PVR_TIMER_TYPE_IS_MANUAL |
                         PVR_TIMER_TYPE_IS_REPEATING |
                         PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS);
    types.emplace_back (type);
  }

  // Repeating EPG.
  {
    kodi::addon::PVRTimerType type;
    //type.SetDescription ("PVR_FREEBOX_GENERATOR_EPG");
    type.SetId (PVR_FREEBOX_GENERATOR_EPG);
    type.SetAttributes ( ATTRIBS |
                         PVR_TIMER_TYPE_IS_REPEATING |
                         PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS |
                         PVR_TIMER_TYPE_REQUIRES_EPG_TAG_ON_CREATE);
    types.emplace_back (type);
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetTimersAmount (int & amount)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  amount = m_generators.size () + m_timers.size ();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetTimers (kodi::addon::PVRTimersResultSet & results)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  //cout << "Freebox::GetTimers" << endl;

#if __cplusplus >= 201703L
  for (auto & [id, g] : m_generators)
#else
  for (auto & it : m_generators)
#endif
  {
#if __cplusplus < 201703L
    int              id = it.first;
    const Generator & g = it.second;
#endif

    kodi::addon::PVRTimer timer;

    time_t now = time (NULL);
    tm today = *localtime (&now);
    today.tm_hour = g.start_hour;
    today.tm_min  = g.start_min;
    today.tm_sec  = 0;
    time_t start = mktime (&today);

    timer.SetTimerType         (PVR_FREEBOX_GENERATOR_MANUAL);
    timer.SetParentClientIndex (PVR_TIMER_NO_PARENT);
    timer.SetClientIndex       (id);
    timer.SetClientChannelUid  (ChannelId (g.channel_uuid));
    timer.SetStartTime         (start);
    timer.SetEndTime           (start + g.duration);
    timer.SetMarginStart       (g.margin_before / 60);
    timer.SetMarginEnd         (g.margin_after  / 60);
    timer.SetWeekdays          ((g.repeat_monday    ? PVR_WEEKDAY_MONDAY    : 0) |
                                (g.repeat_tuesday   ? PVR_WEEKDAY_TUESDAY   : 0) |
                                (g.repeat_wednesday ? PVR_WEEKDAY_WEDNESDAY : 0) |
                                (g.repeat_thursday  ? PVR_WEEKDAY_THURSDAY  : 0) |
                                (g.repeat_friday    ? PVR_WEEKDAY_FRIDAY    : 0) |
                                (g.repeat_saturday  ? PVR_WEEKDAY_SATURDAY  : 0) |
                                (g.repeat_sunday    ? PVR_WEEKDAY_SUNDAY    : 0));
    timer.SetTitle             (g.name);

    results.Add (timer);
  }

#if __cplusplus >= 201703L
  for (auto & [id, t] : m_timers)
#else
  for (auto & it : m_timers)
#endif
  {
#if __cplusplus < 201703L
    int          id = it.first;
    const Timer & t = it.second;
#endif

    kodi::addon::PVRTimer timer;

    if (t.has_record_gen)
    {
      timer.SetTimerType         (PVR_FREEBOX_TIMER_GENERATED);
      timer.SetParentClientIndex (m_unique_id ("generator/" + to_string (t.record_gen_id)));
    }
    else
    {
      timer.SetTimerType         (PVR_FREEBOX_TIMER_MANUAL);
      timer.SetParentClientIndex (PVR_TIMER_NO_PARENT);
    }

    timer.SetClientIndex       (id);
    timer.SetClientChannelUid  (ChannelId (t.channel_uuid));
    timer.SetStartTime         (t.start);
    timer.SetEndTime           (t.end);
    timer.SetMarginStart       (t.margin_before / 60);
    timer.SetMarginEnd         (t.margin_after  / 60);

    /**/ if (t.state == "disabled")           timer.SetState (PVR_TIMER_STATE_DISABLED);
    else if (t.state == "start_error")        timer.SetState (PVR_TIMER_STATE_ERROR);
    else if (t.state == "waiting_start_time") timer.SetState (PVR_TIMER_STATE_SCHEDULED); // FIXME: t.conflict?
    else if (t.state == "starting")           timer.SetState (PVR_TIMER_STATE_RECORDING);
    else if (t.state == "running")            timer.SetState (PVR_TIMER_STATE_RECORDING);
    else if (t.state == "running_error")      timer.SetState (PVR_TIMER_STATE_ERROR);
    else if (t.state == "failed")             timer.SetState (PVR_TIMER_STATE_ERROR);
    else if (t.state == "finished")           timer.SetState (PVR_TIMER_STATE_COMPLETED);

    timer.SetTitle             (t.name);

    results.Add (timer);
  }

  return PVR_ERROR_NO_ERROR;
}

inline Value freebox_generator_weekdays (int w, Document::AllocatorType & a)
{
  Value r (kObjectType);
  r.AddMember ("monday",    (w & PVR_WEEKDAY_MONDAY)    != 0, a);
  r.AddMember ("tuesday",   (w & PVR_WEEKDAY_TUESDAY)   != 0, a);
  r.AddMember ("wednesday", (w & PVR_WEEKDAY_WEDNESDAY) != 0, a);
  r.AddMember ("thursday",  (w & PVR_WEEKDAY_THURSDAY)  != 0, a);
  r.AddMember ("friday",    (w & PVR_WEEKDAY_FRIDAY)    != 0, a);
  r.AddMember ("saturday",  (w & PVR_WEEKDAY_SATURDAY)  != 0, a);
  r.AddMember ("sunday",    (w & PVR_WEEKDAY_SUNDAY)    != 0, a);
  return r;
}

inline Document freebox_generator_request (const kodi::addon::PVRTimer & timer)
{
  string channel_uuid = "uuid-webtv-" + to_string (timer.GetClientChannelUid ());
  string title        = timer.GetTitle ();
  time_t start        = timer.GetStartTime ();
  tm     date         = *localtime (&start);
  int    duration     = timer.GetEndTime () - start;

  Document d (kObjectType);
  Document::AllocatorType & a = d.GetAllocator ();
  d.AddMember ("type", "manual_repeat", a);
  d.AddMember ("name", title,           a);
  Value p (kObjectType);
  p.AddMember ("start_hour",    date.tm_hour, a);
  p.AddMember ("start_min",     date.tm_min,  a);
  p.AddMember ("start_sec",     0,            a);
  p.AddMember ("duration",      duration,     a);
  p.AddMember ("margin_before", timer.GetMarginStart () * 60, a);
  p.AddMember ("margin_after",  timer.GetMarginEnd ()   * 60, a);
  p.AddMember ("channel_uuid",  channel_uuid, a);
  p.AddMember ("repeat_days",   freebox_generator_weekdays (timer.GetWeekdays (), a), a);
  d.AddMember ("params",        p, a);
  return d;
}

PVR_ERROR Freebox::AddTimer (const kodi::addon::PVRTimer & timer)
{
  StartSession ();

  int    type         = timer.GetTimerType ();
  int    channel      = timer.GetClientChannelUid ();
  string channel_uuid = "uuid-webtv-" + to_string (channel);
  string title        = timer.GetTitle ();

  lock_guard<recursive_mutex> lock (m_mutex);
  switch (type)
  {
    case PVR_FREEBOX_TIMER_MANUAL :
    case PVR_FREEBOX_TIMER_EPG :
    {
      //cout << "AddTimer: TIMER[" << type << ']' << endl;

      string subtitle;
      if (timer.GetEPGUid () != EPG_TAG_INVALID_UID)
      {
        Document epg;
        string epg_id = "pluri_" + to_string (timer.GetEPGUid ());
        if (HttpGet ("/api/v6/tv/epg/programs/" + epg_id, &epg))
        {
          Event e (epg ["result"], channel, timer.GetStartTime ());
          ostringstream oss;
          if (e.season  != 0) oss << 'S' << setfill ('0') << setw (2) << e.season;
          if (e.episode != 0) oss << 'E' << setfill ('0') << setw (2) << e.episode;
          string prefix = oss.str ();
          subtitle = (prefix.empty () ? "" : prefix + " - ") + e.subtitle;
        }
      }

      Document d (kObjectType);
      Document::AllocatorType & a = d.GetAllocator ();
      d.AddMember ("start",           (int64_t) timer.GetStartTime (), a);
      d.AddMember ("end",             (int64_t) timer.GetEndTime (),   a);
      d.AddMember ("margin_before",   timer.GetMarginStart () * 60,    a);
      d.AddMember ("margin_after",    timer.GetMarginEnd ()   * 60,    a);
      d.AddMember ("channel_uuid",    channel_uuid,                    a);
      d.AddMember ("channel_type",    "",                              a);
      d.AddMember ("channel_quality", "auto",                          a);
      d.AddMember ("broadcast_type",  "tv",                            a);
      d.AddMember ("name",            title,                           a);
      d.AddMember ("subname",         subtitle,                        a);
    //d.AddMember ("media",           "Disque dur",                    a);
    //d.AddMember ("path",            "Enregistrements",               a);

      // Add timer (Freebox).
      Document response;
      if (! HttpPost ("/api/v6/pvr/programmed/", d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Add timer (locally).
      int id     = response["result"]["id"].GetInt ();
      int unique = m_unique_id ("programmed/" + to_string (id));
      m_timers.emplace (unique, Timer (response["result"]));
      TriggerTimerUpdate ();

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

      // Payload.
      Document d = freebox_generator_request (timer);

      // Add generator (Freebox).
      Document response;
      if (! HttpPost ("/api/v6/pvr/generator/", d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Add generator (locally).
      int id     = response["result"]["id"].GetInt ();
      int unique = m_unique_id ("generator/" + to_string (id));
      m_generators.emplace (unique, Generator (response["result"]));

      // Reload timers.
      ProcessTimers ();
      // Reload recordings.
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

PVR_ERROR Freebox::UpdateTimer (const kodi::addon::PVRTimer& timer)
{
  StartSession ();

  int type = timer.GetTimerType ();

  switch (type)
  {
    case PVR_FREEBOX_TIMER_MANUAL :
    case PVR_FREEBOX_TIMER_EPG :
    {
      lock_guard<recursive_mutex> lock (m_mutex);
      auto i = m_timers.find (timer.GetClientIndex ());
      if (i == m_timers.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "UpdateTimer: TIMER[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      string channel_uuid = "uuid-webtv-" + to_string (timer.GetClientChannelUid ());
      string title        = timer.GetTitle ();

      // Payload.
      Document d (kObjectType);
      Document::AllocatorType & a = d.GetAllocator ();
    //d.AddMember ("enabled",         enabled,                         a);
      d.AddMember ("start",           (int64_t) timer.GetStartTime (), a);
      d.AddMember ("end",             (int64_t) timer.GetEndTime (),   a);
      d.AddMember ("margin_before",   timer.GetMarginStart () * 60,    a);
      d.AddMember ("margin_after",    timer.GetMarginEnd ()   * 60,    a);
      d.AddMember ("channel_uuid",    channel_uuid,                    a);
    //d.AddMember ("channel_type",    "",                              a);
    //d.AddMember ("channel_quality", "auto",                          a);
      d.AddMember ("name",            title,                           a);
    //d.AddMember ("subname",         "",                              a);
    //d.AddMember ("media",           "Disque dur",                    a);
    //d.AddMember ("path",            "Enregistrements",               a);

      // Update timer (Freebox).
      Document response;
      if (! HttpPut ("/api/v6/pvr/programmed/" + to_string (id), d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Update timer (locally).
      i->second = Timer (response["result"]);
      //cout << "UpdateTimer: TIMER[" << type << "]: '" << i->second.state << "'" << endl;
      TriggerTimerUpdate ();

      break;
    }

    case PVR_FREEBOX_TIMER_GENERATED :
    {
      lock_guard<recursive_mutex> lock (m_mutex);
      auto i = m_timers.find (timer.GetClientIndex ());
      if (i == m_timers.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "UpdateTimer: TIMER_GENERATED: " << timer.iClientIndex << " > " << id << endl;

      // Payload.
      Document d (kObjectType);
      d.AddMember ("enabled", timer.GetState () != PVR_TIMER_STATE_DISABLED, d.GetAllocator ());

      // Update generated timer (Freebox).
      Document response;
      if (! HttpPut ("/api/v6/pvr/programmed/" + to_string (id), d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Update generated timer (locally).
      i->second = Timer (response["result"]);
      //cout << "UpdateTimer: TIMER_GENERATED: '" << i->second.state << "'" << endl;
      TriggerTimerUpdate ();

      break;
    }

    case PVR_FREEBOX_GENERATOR_MANUAL :
    case PVR_FREEBOX_GENERATOR_EPG :
    {
      lock_guard<recursive_mutex> lock (m_mutex);
      auto i = m_generators.find (timer.GetClientIndex ());
      if (i == m_generators.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "UpdateTimer: GENERATOR[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      // Payload.
      Document d = freebox_generator_request (timer);

      // Update generator (Freebox).
      Document response;
      if (! HttpPut ("/api/v6/pvr/generator/" + to_string (id), d, &response))
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

PVR_ERROR Freebox::DeleteTimer (const kodi::addon::PVRTimer & timer, bool force)
{
  StartSession ();

  int type = timer.GetTimerType ();

  switch (type)
  {
    case PVR_FREEBOX_TIMER_MANUAL :
    case PVR_FREEBOX_TIMER_EPG :
    {
      lock_guard<recursive_mutex> lock (m_mutex);
      auto i = m_timers.find (timer.GetClientIndex ());
      if (i == m_timers.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "DeleteTimer: TIMER[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      // Delete timer (Freebox).
      Document response;
      if (! HttpDelete ("/api/v6/pvr/programmed/" + to_string (id), &response))
        return PVR_ERROR_SERVER_ERROR;

      // Delete timer (locally).
      m_timers.erase (i);
      TriggerTimerUpdate ();

      // Update recordings if timer was running.
      if (timer.GetState () == PVR_TIMER_STATE_RECORDING)
        ProcessRecordings ();

      break;
    }

    case PVR_FREEBOX_GENERATOR_MANUAL :
    case PVR_FREEBOX_GENERATOR_EPG :
    {
      lock_guard<recursive_mutex> lock (m_mutex);
      auto i = m_generators.find (timer.GetClientIndex ());
      if (i == m_generators.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "DeleteTimer: GENERATOR[" << type << "]: " << timer.GetClientIndex () << " > " << id << endl;

      // Delete generator (Freebox).
      Document response;
      if (! HttpDelete ("/api/v6/pvr/generator/" + to_string (id), &response))
        return PVR_ERROR_SERVER_ERROR;

      // Delete generated timers (locally).
      for (auto i = m_timers.begin (); i != m_timers.end ();)
        if (i->second.record_gen_id == id)
          i = m_timers.erase (i);
        else
          ++i;

      // Delete generator (locally).
      m_generators.erase (i);
      TriggerTimerUpdate ();

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

////////////////////////////////////////////////////////////////////////////////
// H O O K S ///////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

/* static */
enum Freebox::Source Freebox::DialogSource (enum Source selected)
{
  // Heading.
  string heading = kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_SOURCE);

  // Entries.
  const vector<string> LABELS =
  {
    kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_SOURCE_AUTO),
    kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_SOURCE_IPTV),
    kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_SOURCE_DVB)
  };

  return (Source) kodi::gui::dialogs::Select::Show (heading, LABELS, (int) selected);
}

/* static */
enum Freebox::Quality Freebox::DialogQuality (enum Quality selected)
{
  // Heading.
  string heading = kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_QUALITY);

  // Entries.
  const vector<string> LABELS =
  {
    kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_QUALITY_AUTO),
    kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_QUALITY_HD),
    kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_QUALITY_SD),
    kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_QUALITY_LD),
    kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNEL_QUALITY_3D)
  };

  return (Quality) kodi::gui::dialogs::Select::Show (heading, LABELS, (int) selected);
}

PVR_ERROR Freebox::CallChannelMenuHook (const kodi::addon::PVRMenuhook & menuhook, const kodi::addon::PVRChannel & item)
{
  switch (menuhook.GetHookId ())
  {
    case PVR_FREEBOX_MENUHOOK_CHANNEL_SOURCE:
    {
      unsigned int id = item.GetUniqueId ();
      SetChannelSource (id, DialogSource (ChannelSource (id, false)));

      return PVR_ERROR_NO_ERROR;
    }

    case PVR_FREEBOX_MENUHOOK_CHANNEL_QUALITY:
    {
      unsigned int id = item.GetUniqueId ();
      SetChannelQuality (id, DialogQuality (ChannelQuality (id, false)));

      return PVR_ERROR_NO_ERROR;
    }
  }

  return PVR_ERROR_NO_ERROR;
}

ADDONCREATOR(Freebox)
