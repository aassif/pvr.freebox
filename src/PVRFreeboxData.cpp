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
#include <string>
#include <algorithm>

#include "client.h"
#include "PVRFreeboxData.h"
#include "p8-platform/util/StringUtils.h"

using namespace std;
using namespace rapidjson;
using namespace ADDON;

#define PVR_FREEBOX_C_STR(s) s.empty () ? NULL : s.c_str ()

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
    inline bool operator() (const Conflict & c1, const Conflict & c2)
    {
      return (c1.major < c2.major || c1.major == c2.major && c1.minor < c2.minor);
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

PVRFreeboxData::Stream::Stream (enum Quality quality,
                                const string & url) :
  quality (quality),
  url (url)
{
}

enum PVRFreeboxData::Quality PVRFreeboxData::Channel::ParseQuality (const string & q)
{
  if (q == "auto") return AUTO;
  if (q == "hd")   return HD;
  if (q == "sd")   return SD;
  if (q == "ld")   return LD;
  return DEFAULT;
}

int PVRFreeboxData::Channel::Score (enum Quality q, enum Quality q0)
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

    default:
      return 0;
  }
}

PVRFreeboxData::Channel::Channel (const string & uuid,
                                  const string & name,
                                  const string & logo,
                                  int major, int minor,
                                  const Value & item) :
  radio (false),
  uuid (uuid),
  name (name),
  logo (logo),
  major (major), minor (minor),
  streams ()
{
  if (item.HasMember("available") && item["available"].GetBool () && item.HasMember("streams"))
  {
    const Value & streams = item["streams"];
    if (streams.IsArray ())
    {
      for (SizeType i = 0; i < streams.Size (); ++i)
      {
        const Value  & s = streams [i];
        const string & q = s["quality"].GetString ();
        const string & r = s["rtsp"].GetString ();
        this->streams.emplace_back (ParseQuality (q), r);
      }
    }
  }
}

void PVRFreeboxData::Channel::GetChannel (ADDON_HANDLE handle, bool radio) const
{
  PVR_CHANNEL channel;
  memset (&channel, 0, sizeof (PVR_CHANNEL));

  channel.iUniqueId         = ChannelId (uuid);
  channel.bIsRadio          = false;
  channel.iChannelNumber    = major;
  channel.iSubChannelNumber = minor;
  strncpy (channel.strChannelName, name.c_str (), PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy (channel.strIconPath,    logo.c_str (), PVR_ADDON_URL_STRING_LENGTH  - 1);
  channel.bIsHidden         = streams.empty ();

  PVR->TransferChannelEntry (handle, &channel);
}

PVR_ERROR PVRFreeboxData::Channel::GetStreamProperties (enum Quality q, PVR_NAMED_VALUE * properties, unsigned int * count) const
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

bool PVRFreeboxData::ReadJSON (Document * doc, const string & url)
{
  void * file = XBMC->OpenFile (url.c_str (), XFILE::READ_NO_CACHE);

  if (file)
  {
    string data;

    char buffer [1024];
    while (int bytes = XBMC->ReadFile (file, buffer, 1024))
      data.append (buffer, bytes);

    XBMC->CloseFile (file);

    return ! doc->Parse (data.c_str ()).HasParseError ();
  }

  return false;
}

PVRFreeboxData::Event::Event (const Value & e, unsigned int channel) :
  channel  (channel),
  uuid     (JSON<string> (e, "id")),
  date     (JSON<int>    (e, "date")),
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

bool PVRFreeboxData::ProcessChannels ()
{
  m_tv_channels.clear ();

  Document channels;
  if (! ReadJSON (&channels, URL ("/api/v5/tv/channels")))                  return false;
  if (! channels.HasMember ("success") || ! channels["success"].GetBool ()) return false;
  if (! channels.HasMember ("result")  || ! channels["result"].IsObject ()) return false;

  char * notification = XBMC->GetLocalizedString (30000); // "%d channels loaded"
  XBMC->QueueNotification (QUEUE_INFO, notification, channels["result"].MemberCount ());
  XBMC->FreeString (notification);

  //Document bouquets;
  //ReadJSON (&m_tv_bouquets, URL ("/api/v5/tv/bouquets"));

  Document bouquet;
  if (! ReadJSON (&bouquet, URL ("/api/v5/tv/bouquets/freeboxtv/channels"))) return false;
  if (! bouquet.HasMember ("success") || ! bouquet["success"].GetBool ())    return false;
  if (! bouquet.HasMember ("result")  || ! bouquet["result"].IsArray ())     return false;

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

    channels_by_uuid [uuid] .push_back (c);
    channels_by_major[major].push_back (c);
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
      const Value  & channel = channels["result"][ch.uuid.c_str ()];
      const string & name    = channel["name"].GetString ();
      const string & logo    = URL (channel["logo_url"].GetString ());
      const Value  & item    = bouquet["result"][ch.position];
      m_tv_channels.emplace (ChannelId (ch.uuid), Channel (ch.uuid, name, logo, ch.major, ch.minor, item));
    }
  }

  return true;
}

PVRFreeboxData::PVRFreeboxData (const string & path,
                                int quality,
                                int days,
                                bool extended,
                                int delay) :
  m_path (path),
  m_server ("mafreebox.freebox.fr"),
  m_delay (delay),
  m_tv_channels (),
  m_tv_quality (Quality (quality)),
  m_epg_queries (),
  m_epg_cache (),
  m_epg_events (),
  m_epg_days (0),
  m_epg_last (0),
  m_epg_extended (extended)
{
  SetDays (days);
  ProcessChannels ();
  CreateThread (false);
}

PVRFreeboxData::~PVRFreeboxData ()
{
  StopThread ();
}

string PVRFreeboxData::GetServer () const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_server;
}

// NOT thread-safe !
string PVRFreeboxData::URL (const string & query) const
{
  return "http://" + m_server + query;
}

void PVRFreeboxData::SetQuality (int q)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_tv_quality = Quality (q);
}

void PVRFreeboxData::SetDays (int d)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_epg_days = d != EPG_TIMEFRAME_UNLIMITED ? min (d, 7) : 7;
}

void PVRFreeboxData::SetExtended (bool e)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_epg_extended = e;
}

void PVRFreeboxData::SetDelay (int d)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_delay = d;
}

void PVRFreeboxData::ProcessEvent (const Event & e, EPG_EVENT_STATE state)
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

void PVRFreeboxData::ProcessEvent (const Value & event, unsigned int channel, EPG_EVENT_STATE state)
{
  switch (state)
  {
    case EPG_EVENT_CREATED:
    {
      Event e (event, channel);
      {
        P8PLATFORM::CLockObject lock (m_mutex);
        if (m_epg_extended)
        {
          string query = "/api/v5/tv/epg/programs/" + e.uuid;
          m_epg_events.insert (make_pair (e.uuid, e));
          m_epg_queries.push (Query (EVENT, URL (query), channel));
          //XBMC->Log (LOG_INFO, "Queued: '%s'", query.c_str ());
        }
      }
      ProcessEvent (e, EPG_EVENT_CREATED);
      break;
    }

    case EPG_EVENT_UPDATED:
    {
      Event e (event, channel);
      {
        P8PLATFORM::CLockObject lock (m_mutex);
        auto f = m_epg_events.find (e.uuid);
        if (f != m_epg_events.end ())
        {
          e.date = f->second.date; // !!!
          m_epg_events.erase (f);
        }
      }
      ProcessEvent (e, EPG_EVENT_UPDATED);
      break;
    }
  }
}

void PVRFreeboxData::ProcessChannel (const Value & epg, unsigned int channel)
{
  for (auto i = epg.MemberBegin (); i != epg.MemberEnd (); ++i)
  {
    const Value & event = i->value;
    string uuid = JSON<string> (event, "id");

    static const string PREFIX = "pluri_";
    if (uuid.find (PREFIX) != 0) continue;

    string query = "/api/v5/tv/epg/programs/" + uuid;

    {
      P8PLATFORM::CLockObject lock (m_mutex);
      if (m_epg_cache.count (query) > 0) continue;
    }

    ProcessEvent (event, channel, EPG_EVENT_CREATED);

    {
      P8PLATFORM::CLockObject lock (m_mutex);
      m_epg_cache.insert (query);
    }
  }
}

void PVRFreeboxData::ProcessFull (const Value & epg)
{
  for (auto i = epg.MemberBegin (); i != epg.MemberEnd (); ++i)
  {
    string uuid = i->name.GetString ();
    ProcessChannel (i->value, ChannelId (uuid));
  }
}

void * PVRFreeboxData::Process ()
{
  while (! IsStopped ())
  {
    int    delay = 0;
    int    days  = 1;
    time_t last  = 0;
    time_t now   = time (NULL);
    time_t end   = now + days * 24 * 60 * 60;

    {
      P8PLATFORM::CLockObject lock (m_mutex);
      delay = m_delay;
      days  = m_epg_days;
      last  = max (now, m_epg_last);
    }

    for (time_t t = last - (last % 3600); t < end; t += 3600)
    {
      string epoch = to_string (t);
      string query = "/api/v5/tv/epg/by_time/" + epoch;
      {
        P8PLATFORM::CLockObject lock (m_mutex);
        m_epg_queries.push (Query (FULL, URL (query)));
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
      if (ReadJSON (&json, q.query) && json["success"].GetBool ())
      {
        const Value & r = json["result"];
        if (r.IsObject ())
        {
          switch (q.type)
          {
            case FULL    : ProcessFull    (r); break;
            case CHANNEL : ProcessChannel (r, q.channel); break;
            case EVENT   : ProcessEvent   (r, q.channel, EPG_EVENT_UPDATED); break;
          }
        }
      }
    }

    Sleep (delay * 1000);
  }

  return NULL;
}

int PVRFreeboxData::GetChannelsAmount ()
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_tv_channels.size ();
}

PVR_ERROR PVRFreeboxData::GetChannels (ADDON_HANDLE handle, bool radio)
{
  P8PLATFORM::CLockObject lock (m_mutex);

  //for (auto i = m_tv_channels.begin (); i != m_tv_channels.end (); ++i)
  for (auto i : m_tv_channels)
    i.second.GetChannel (handle, radio);

  return PVR_ERROR_NO_ERROR;
}

int PVRFreeboxData::GetChannelGroupsAmount ()
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return 0;
}

PVR_ERROR PVRFreeboxData::GetChannelGroups (ADDON_HANDLE handle, bool radio)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRFreeboxData::GetChannelGroupMembers (ADDON_HANDLE handle, const PVR_CHANNEL_GROUP & group)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRFreeboxData::GetChannelStreamProperties (const PVR_CHANNEL * channel, PVR_NAMED_VALUE * properties, unsigned int * count)
{
  if (!channel || !properties || !count)
    return PVR_ERROR_SERVER_ERROR;

  if (*count < 2)
    return PVR_ERROR_INVALID_PARAMETERS;

  P8PLATFORM::CLockObject lock (m_mutex);
  auto f = m_tv_channels.find (channel->iUniqueId);
  if (f != m_tv_channels.end ())
    return f->second.GetStreamProperties (m_tv_quality, properties, count);

  return PVR_ERROR_NO_ERROR;
}

