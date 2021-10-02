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

#ifdef CreateDirectory
#undef CreateDirectory
#endif // CreateDirectory

using namespace std;
using json = nlohmann::json;

#define PVR_FREEBOX_TIMER_MANUAL     1
#define PVR_FREEBOX_TIMER_EPG        2
#define PVR_FREEBOX_TIMER_GENERATED  3
#define PVR_FREEBOX_GENERATOR_MANUAL 4
#define PVR_FREEBOX_GENERATOR_EPG    5

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
                    const json & request,
                    json * result,
                    json::value_t type) const
{
  m_mutex.lock ();
  string url = URL (path);
  string session = m_session_token;
  m_mutex.unlock ();

  string response;
  long http = freebox_http (custom, url, request.dump (), &response, session);
  kodi::Log (ADDON_LOG_DEBUG, "%s %s %s", custom.c_str (), url.c_str (), response.c_str ());

  json j = json::parse (response, nullptr, false);

  if (! j.is_object ()) return false;

  if (! j.value ("success", false)) return false;

  if (result != nullptr)
  {
    auto r = j.find ("result");
    if (r == j.end ()) return false;

    if (r->type () != type) return false;

    *result = *r;
  }

  if (http != 200)
  {
    kodi::QueueFormattedNotification (QUEUE_INFO, "HTTP %d", http);
    cout << "HTTP " << http << " : " << response << endl;
    return false;
  }

  return true;
}

/* static */
bool Freebox::HttpGet (const string & path,
                       json * result,
                       json::value_t type) const
{
  return Http ("GET", path, json (), result, type);
}

/* static */
bool Freebox::HttpPost (const string & path,
                        const json & request,
                        json * result,
                        json::value_t type) const
{
  return Http ("POST", path, request, result, type);
}

/* static */
bool Freebox::HttpPut (const string & path,
                       const json & request,
                       json * result,
                       json::value_t type) const
{
  return Http ("PUT", path, request, result, type);
}

/* static */
bool Freebox::HttpDelete (const string & path) const
{
  return Http ("DELETE", path, json (), nullptr, json::value_t::null);
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

      json request =
      {
        {"app_id",      PVR_FREEBOX_APP_ID},
        {"app_name",    PVR_FREEBOX_APP_NAME},
        {"app_version", PVR_FREEBOX_APP_VERSION},
        {"device_name", hostname}
      };

      json result;
      if (! HttpPost ("/api/v6/login/authorize", request, &result)) return false;
      m_app_token = result.value ("app_token", "");
      m_track_id  = result.value ("track_id", 0);

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

  json login;
  if (! HttpGet ("/api/v6/login/", &login))
    return false;

  if (! login.value ("logged_in", false))
  {
    json d;
    string track = to_string (m_track_id);
    string url   = "/api/v6/login/authorize/" + track;
    if (! HttpGet (url, &d)) return false;
    string status    = d.value ("status", "");
    string challenge = d.value ("challenge", "");
    //string salt      = d.value ("password_salt", "");
    //cout << status << ' ' << challenge << ' ' << salt << endl;

    if (status == "granted")
    {
      string password = Password (m_app_token, challenge);
      //cout << "password: " << password << " [" << password.length () << ']' << endl;

      json request =
      {
        {"app_id",   PVR_FREEBOX_APP_ID},
        {"password", password}
      };

      json result;
      if (! HttpPost ("/api/v6/login/session", request, &result)) return false;
      m_session_token = result.value ("session_token", "");

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
    return HttpPost ("/api/v6/login/logout/", json (), nullptr);

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

Freebox::Event::CastMember::CastMember (const json & c) :
  job        (c.value ("job", "")),
  first_name (c.value ("first_name", "")),
  last_name  (c.value ("last_name", "")),
  role       (c.value ("role", ""))
{
}

Freebox::Event::Event (const json & e, unsigned int channel, time_t date) :
  channel  (channel),
  uuid     (e.value ("id", "")),
  date     (e.value ("date", date)),
  duration (e.value ("duration", 0)),
  title    (e.value ("title", "")),
  subtitle (e.value ("sub_title", "")),
  season   (e.value ("season_number", 0)),
  episode  (e.value ("episode_number", 0)),
  category (e.value ("category", 0)),
  picture  (e.value ("picture_big", e.value ("picture", ""))),
  plot     (e.value ("desc", "")),
  outline  (e.value ("short_desc", "")),
  year     (e.value ("year", 0)),
  cast     ()
{
  if (category != 0 && Colors (category) == 0)
  {
    string name = e.value ("category_name", "");
    cout << category << " : " << name << endl;
  }

  auto f = e.find ("cast");
  if (f != e.end ())
    if (f->is_array ())
      for (auto & c : *f)
        cast.emplace_back (c);
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

  json channels;
  if (! HttpGet ("/api/v6/tv/channels", &channels)) return false;

  string notification = kodi::GetLocalizedString (PVR_FREEBOX_STRING_CHANNELS_LOADED);
  kodi::QueueFormattedNotification (QUEUE_INFO, notification.c_str (), channels.size ());

  //json bouquets;
  //HttpGet ("/api/v6/tv/bouquets", &m_tv_bouquets);

  json bouquet;
  if (! HttpGet ("/api/v6/tv/bouquets/freeboxtv/channels", &bouquet, json::value_t::array)) return false;

  // Conflict list.
  typedef vector<Conflict> Conflicts;
  // Conflicts by UUID.
  map<string, Conflicts> conflicts_by_uuid;
  // Conflicts by major.
  map<int, Conflicts> conflicts_by_major;

  for (int i = 0; i < bouquet.size (); ++i)
  {
    string uuid  = bouquet[i]["uuid"];
    int    major = bouquet[i]["number"];
    int    minor = bouquet[i]["sub_number"];

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
  for (auto & i : conflicts_by_major)
    cout << i.first << " : " << StrUUIDs (i.second) << endl;
#endif

#if 0
  for (auto & i : conflicts_by_uuid)
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
      const json   & channel = channels[ch.uuid];
      const string & name    = channel["name"];
      const string & logo    = URL (channel["logo_url"]);
      const json   & item    = bouquet[ch.position];

      vector<Stream> data;
      if (item.value ("available", false))
      {
        auto f = item.find ("streams");
        if (f != item.end () && f->is_array ())
          for (auto & s : *f)
            data.emplace_back (ParseSource (s["type"]),
                               ParseQuality (s["quality"]),
                               freebox_replace_server (s["rtsp"], m_hostname),
                               freebox_replace_server (s.value ("hls", ""), m_hostname));
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
    ifstream ifs (m_path + "source.txt");
    json d = json::parse (ifs, nullptr, false);
    if (d.is_object ())
    {
      for (auto & item : d.items ())
        m_tv_prefs_source.emplace (ChannelId (item.key ()), ParseSource (item.value ()));
    }
  }

  {
    ifstream ifs (m_path + "quality.txt");
    json d = json::parse (ifs, nullptr, false);
    if (d.is_object ())
    {
      for (auto & item : d.items ())
        m_tv_prefs_quality.emplace (ChannelId (item.key ()), ParseQuality (item.value ()));
    }
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
  m_epg_days_past (0),
  m_epg_days_future (0),
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

void Freebox::SetHostName (const string & hostname)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_hostname = hostname;
}

string Freebox::GetHostName () const
{
  lock_guard<recursive_mutex> lock (m_mutex);
  return m_hostname;
}

void Freebox::SetNetBIOS (const string & netbios)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_netbios = netbios;
}

string Freebox::GetNetBIOS () const
{
  lock_guard<recursive_mutex> lock (m_mutex);
  return m_netbios;
}

// NOT thread-safe !
string Freebox::URL (const string & query) const
{
  return "http://" + m_hostname + query;
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

void Freebox::SetPastDays (int d)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_epg_days_past = d != EPG_TIMEFRAME_UNLIMITED ? min (d, 7) : 7;
}

void Freebox::SetFutureDays (int d)
{
  lock_guard<recursive_mutex> lock (m_mutex);
  m_epg_days_future = d != EPG_TIMEFRAME_UNLIMITED ? min (d, 7) : 7;
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

void Freebox::ProcessEvent (const json & event, unsigned int channel, time_t date, EPG_EVENT_STATE state)
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

void Freebox::ProcessChannel (const json & epg, unsigned int channel)
{
  for (auto & event : epg)
  {
    string uuid = event.value ("id", "");
    time_t date = event.value ("date", 0);

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

void Freebox::ProcessFull (const json & epg)
{
  for (auto & item : epg.items ())
    ProcessChannel (item.value (), ChannelId (item.key ()));
}

void Freebox::Process ()
{
  while (! m_threadStop )
  {
    m_mutex.lock ();
    int    delay = m_delay;
    time_t now   = time (NULL);
    time_t begin = now - m_epg_days_past   * 24 * 3600;
    time_t end   = now + m_epg_days_future * 24 * 3600;
    time_t last  = max (begin, m_epg_last);
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

      json result;
      if (HttpGet (q.query, &result))
      {
        switch (q.type)
        {
          case FULL    : ProcessFull    (result); break;
          case CHANNEL : ProcessChannel (result, q.channel); break;
          case EVENT   : ProcessEvent   (result, q.channel, q.date, EPG_EVENT_UPDATED); break;
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
  SetPastDays (EpgMaxPastDays ());
  SetFutureDays (EpgMaxFutureDays ());
  ProcessChannels ();
  CreateThread ();

  return ADDON_STATUS_OK;
}

ADDON_STATUS Freebox::SetSetting (const string & settingName, const kodi::CSettingValue & settingValue)
{
  /**/ if (settingName == "hostname")
  {
    SetHostName (settingValue.GetString ());
    return ADDON_STATUS_NEED_RESTART;
  }

  else if (settingName == "netbios")
  {
    SetNetBIOS (settingValue.GetString ());
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
  m_hostname     = kodi::GetSettingString         ("hostname", PVR_FREEBOX_DEFAULT_HOSTNAME);
  m_netbios      = kodi::GetSettingString         ("netbios",  PVR_FREEBOX_DEFAULT_NETBIOS);
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
  caps.SetSupportsRecordingsDelete         (true);
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
  hostname = GetHostName ();
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

PVR_ERROR Freebox::SetEPGMaxPastDays (int days)
{
  SetPastDays (days);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::SetEPGMaxFutureDays (int days)
{
  SetFutureDays (days);
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

  json d;
  for (auto & i : m_tv_prefs_source)
    d.emplace ("uuid-webtv-" + to_string (i.first), StrSource (i.second));

  ofstream ofs (m_path + "source.txt");
  ofs << d;
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

  json d;
  for (auto & i : m_tv_prefs_quality)
    d.emplace ("uuid-webtv-" + to_string (i.first), StrQuality (i.second));

  ofstream ofs (m_path + "quality.txt");
  ofs << d;
}

////////////////////////////////////////////////////////////////////////////////
// R E C O R D I N G S /////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

Freebox::Recording::Recording (const json & r) :
  id              (r.value ("id", -1)),
  start           (r.value ("start", 0)),
  end             (r.value ("end", 0)),
  name            (r.value ("name", "")),
  subname         (r.value ("subname", "")),
  channel_uuid    (r.value ("channel_uuid", "")),
  channel_name    (r.value ("channel_name", "")),
//channel_quality (r.value ("channel_quality", "")),
//channel_type    (r.value ("channel_type", "")),
//broadcast_type  (r.value ("broadcast_type", "")),
  media           (r.value ("media", "")),
  path            (r.value ("path", "")),
  filename        (r.value ("filename", "")),
  byte_size       (r.value ("byte_size", 0)),
  secure          (r.value ("secure", false))
{
}

void Freebox::ProcessRecordings ()
{
  m_recordings.clear ();

  json recordings;
  if (HttpGet ("/api/v6/pvr/finished/", &recordings, json::value_t::array))
  {
    for (auto & r : recordings)
      m_recordings.emplace (r.value ("id", -1), Recording (r));

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
  string stream = "smb://" + m_netbios + '/' + r.media + '/' + r.path + '/' + r.filename;
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
  json d = {{"name", name}, {"subname", subname}};

  // Update recording (Freebox).
  json result;
  if (! HttpPut ("/api/v6/pvr/finished/" + to_string (id), d, &result))
    return PVR_ERROR_SERVER_ERROR;

  // Update recording (locally).
  i->second = Recording (result);
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
  if (! HttpDelete ("/api/v6/pvr/finished/" + to_string (id)))
    return PVR_ERROR_SERVER_ERROR;

  // Delete recording (locally).
  m_recordings.erase (i);
  TriggerRecordingUpdate ();

  return PVR_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
// T I M E R S /////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

Freebox::Generator::Generator (const json & g) :
  id               (g.value ("id", -1)),
//type             (g.value ("type", "")),
  media            (g.value ("media", "")),
  path             (g.value ("path", "")),
  name             (g.value ("name", "")),
//subname          (g.value ("name", "")),
  channel_uuid     (g.value ("/params/channel_uuid"_json_pointer, "")),
//channel_type     (g.value ("/params/channel_type"_json_pointer, "")),
//channel_quality  (g.value ("/params/channel_quality"_json_pointer, "")),
//channel_strict   (g.value ("/params/channel_strict"_json_pointer, ""))
//broadcast_type   (g.value ("/params/broadcast_type"_json_pointer, ""))
  start_hour       (g.value ("/params/start_hour"_json_pointer, 0)),
  start_min        (g.value ("/params/start_min"_json_pointer, 0)),
  duration         (g.value ("/params/duration"_json_pointer, 0)),
  margin_before    (g.value ("/params/margin_before"_json_pointer, 0)),
  margin_after     (g.value ("/params/margin_after"_json_pointer, 0)),
  repeat_monday    (g.value ("/params/repeat_days/monday"_json_pointer, false)),
  repeat_tuesday   (g.value ("/params/repeat_days/tuesday"_json_pointer, false)),
  repeat_wednesday (g.value ("/params/repeat_days/wednesday"_json_pointer, false)),
  repeat_thursday  (g.value ("/params/repeat_days/thursday"_json_pointer, false)),
  repeat_friday    (g.value ("/params/repeat_days/friday"_json_pointer, false)),
  repeat_saturday  (g.value ("/params/repeat_days/saturday"_json_pointer, false)),
  repeat_sunday    (g.value ("/params/repeat_days/sunday"_json_pointer,  false))
{
}

void Freebox::ProcessGenerators ()
{
  m_generators.clear ();

  json generators;
  if (HttpGet ("/api/v6/pvr/generator/", &generators, json::value_t::array))
  {
    for (auto & g : generators)
    {
      int        id = g.value ("id", -1);
      int unique_id = m_unique_id ("generator/" + to_string (id));
      m_generators.emplace (unique_id, Generator (g));
    }

    TriggerTimerUpdate ();
  }
}

Freebox::Timer::Timer (const json & t) :
  id             (t.value ("id", -1)),
  start          (t.value ("start", 0)),
  end            (t.value ("end", 0)),
  margin_before  (t.value ("margin_before", 0)),
  margin_after   (t.value ("margin_after", 0)),
  name           (t.value ("name", "")),
  subname        (t.value ("subname", "")),
  channel_uuid   (t.value ("channel_uuid", "")),
  channel_name   (t.value ("channel_name", "")),
//channel_type   (t.value ("channel_type", "")),
//broadcast_type (t.value ("broadcast_type", "")),
  media          (t.value ("media", "")),
  path           (t.value ("path", "")),
  has_record_gen (t.value ("has_record_gen", false)),
  record_gen_id  (t.value ("record_gen_id", 0)),
  enabled        (t.value ("enabled", false)),
  conflict       (t.value ("conflict", false)),
  state          (t.value ("state", "disabled")),
  error          (t.value ("error", "none"))
{
}

void Freebox::ProcessTimers ()
{
  m_timers.clear ();

  json timers;
  if (HttpGet ("/api/v6/pvr/programmed/", &timers, json::value_t::array))
  {
    for (auto & t : timers)
    {
      int        id = t.value ("id", -1);
      int unique_id = m_unique_id ("programmed/" + to_string (id));

      const string & state = t.value ("state", "disabled");
      if (state != "finished" && state != "failed" && state != "start_error" && state != "running_error")
        m_timers.emplace (unique_id, Timer (t));
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

inline json freebox_generator_weekdays (int w)
{
  return json::object ({
    {"monday",    (w & PVR_WEEKDAY_MONDAY)    != 0},
    {"tuesday",   (w & PVR_WEEKDAY_TUESDAY)   != 0},
    {"wednesday", (w & PVR_WEEKDAY_WEDNESDAY) != 0},
    {"thursday",  (w & PVR_WEEKDAY_THURSDAY)  != 0},
    {"friday",    (w & PVR_WEEKDAY_FRIDAY)    != 0},
    {"saturday",  (w & PVR_WEEKDAY_SATURDAY)  != 0},
    {"sunday",    (w & PVR_WEEKDAY_SUNDAY)    != 0}
  });
}

inline json freebox_generator_request (const kodi::addon::PVRTimer & timer)
{
  string channel_uuid = "uuid-webtv-" + to_string (timer.GetClientChannelUid ());
  string title        = timer.GetTitle ();
  time_t start        = timer.GetStartTime ();
  tm     date         = *localtime (&start);
  int    duration     = timer.GetEndTime () - start;

  return json::object ({
    {"type", "manual_repeat"},
    {"name", title},
    {"params", {
      {"start_hour",    date.tm_hour},
      {"start_min",     date.tm_min},
      {"start_sec",     0},
      {"duration",      duration},
      {"margin_before", 60 * timer.GetMarginStart ()},
      {"margin_after",  60 * timer.GetMarginEnd ()},
      {"channel_uuid",  channel_uuid},
      {"repeat_days",   freebox_generator_weekdays (timer.GetWeekdays ())}
  }}});
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
        json epg;
        string epg_id = "pluri_" + to_string (timer.GetEPGUid ());
        if (HttpGet ("/api/v6/tv/epg/programs/" + epg_id, &epg))
        {
          Event e (epg, channel, timer.GetStartTime ());
          ostringstream oss;
          if (e.season  != 0) oss << 'S' << setfill ('0') << setw (2) << e.season;
          if (e.episode != 0) oss << 'E' << setfill ('0') << setw (2) << e.episode;
          string prefix = oss.str ();
          subtitle = (prefix.empty () ? "" : prefix + " - ") + e.subtitle;
        }
      }

      json d = {
        {"start",           (int64_t) timer.GetStartTime ()},
        {"end",             (int64_t) timer.GetEndTime ()},
        {"margin_before",   60 * timer.GetMarginStart ()},
        {"margin_after",    60 * timer.GetMarginEnd ()},
        {"channel_uuid",    channel_uuid},
        {"channel_type",    ""},
        {"channel_quality", "auto"},
        {"broadcast_type",  "tv"},
        {"name",            title},
        {"subname",         subtitle}
      };
      //{"media",           "Disque dur"},
      //{"path",            "Enregistrements"},

      // Add timer (Freebox).
      json result;
      if (! HttpPost ("/api/v6/pvr/programmed/", d, &result))
        return PVR_ERROR_SERVER_ERROR;

      // Add timer (locally).
      int id     = result.value ("id", -1);
      int unique = m_unique_id ("programmed/" + to_string (id));
      m_timers.emplace (unique, Timer (result));
      TriggerTimerUpdate ();

      // Update recordings if timer is running.
      string state = result.value ("state", "disabled");
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
      json d = freebox_generator_request (timer);

      // Add generator (Freebox).
      json result;
      if (! HttpPost ("/api/v6/pvr/generator/", d, &result))
        return PVR_ERROR_SERVER_ERROR;

      // Add generator (locally).
      int id     = result.value ("id", -1);
      int unique = m_unique_id ("generator/" + to_string (id));
      m_generators.emplace (unique, Generator (result));

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
      json d =
      {
      //{"enabled",         enabled},
        {"start",           (int64_t) timer.GetStartTime ()},
        {"end",             (int64_t) timer.GetEndTime ()},
        {"margin_before",   60 * timer.GetMarginStart ()},
        {"margin_after",    60 * timer.GetMarginEnd ()},
        {"channel_uuid",    channel_uuid},
      //{"channel_type",    ""},
      //{"channel_quality", "auto"},
        {"name",            title}
      //{"subname",         ""},
      //{"media",           "Disque dur"},
      //{"path",            "Enregistrements"},
      };

      // Update timer (Freebox).
      json result;
      if (! HttpPut ("/api/v6/pvr/programmed/" + to_string (id), d, &result))
        return PVR_ERROR_SERVER_ERROR;

      // Update timer (locally).
      i->second = Timer (result);
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
      json d = {{"enabled", timer.GetState () != PVR_TIMER_STATE_DISABLED}};

      // Update generated timer (Freebox).
      json result;
      if (! HttpPut ("/api/v6/pvr/programmed/" + to_string (id), d, &result))
        return PVR_ERROR_SERVER_ERROR;

      // Update generated timer (locally).
      i->second = Timer (result);
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
      json d = freebox_generator_request (timer);

      // Update generator (Freebox).
      json result;
      if (! HttpPut ("/api/v6/pvr/generator/" + to_string (id), d, &result))
        return PVR_ERROR_SERVER_ERROR;

      // Update generator (locally).
      i->second = Generator (result);
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
      if (! HttpDelete ("/api/v6/pvr/programmed/" + to_string (id)))
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
      if (! HttpDelete ("/api/v6/pvr/generator/" + to_string (id)))
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
