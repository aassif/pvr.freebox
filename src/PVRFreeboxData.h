#pragma once
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

#include <set>
#include <map>
#include <queue>
#include "libXBMC_pvr.h"
#include "p8-platform/os.h"
#include "p8-platform/threads/threads.h"
#include "rapidjson/document.h"

class PVRFreeboxData :
  public P8PLATFORM::CThread
{
  protected:
    inline static unsigned int ChannelId (const std::string & uuid)
    {
      return std::stoi (uuid.substr (11)); // uuid-webtv-*
    }

    inline static unsigned int BroadcastId (const std::string & uuid)
    {
      return std::stoi (uuid.substr (6)); // pluri_*
    }

    // Channel quality.
    enum Quality {DEFAULT = 0, AUTO = 1, HD = 2, SD = 3, LD = 4};

    class Stream
    {
      public:
        enum Quality quality;
        std::string  url;

      public:
        Stream (enum Quality, const std::string &);
    };

    class Channel
    {
      public:
        typedef std::map<enum Quality, std::string> Streams;

      protected:
        static enum Quality ParseQuality (const std::string &);
        static int Score (enum Quality q, enum Quality q0);

      protected:
        bool                radio;
        std::string         uuid;
        std::string         name;
        std::string         logo;
        int                 major;
        int                 minor;
        std::vector<Stream> streams;

      public:
        Channel (const std::string & uuid,
                 const std::string & name,
                 const std::string & logo,
                 int major, int minor,
                 const rapidjson::Value & item);

        void GetChannel (ADDON_HANDLE, bool radio) const;
        PVR_ERROR GetStreamProperties (enum Quality, PVR_NAMED_VALUE *, unsigned int * count) const;
    };

    // Query types.
    enum QueryType {NONE = 0, FULL = 1, CHANNEL = 2, EVENT = 3};

    class Query
    {
      public:
        QueryType    type;
        std::string  query;
        unsigned int channel;
        time_t       date;

      public:
        Query () : type (NONE) {}

        Query (QueryType t,
               const std::string & q,
               unsigned int c = 0,
               time_t d = 0) :
          type (t),
          query (q),
          channel (c),
          date (d)
        {
        }
    };

    // EPG events.
    class Event
    {
      public:
        unsigned int channel;
        std::string  uuid;
        time_t       date;
        int          duration;
        std::string  title;
        std::string  subtitle;
        int          season;
        int          episode;
        std::string  category;
        std::string  picture;
        std::string  plot;
        std::string  outline;

      public:
        Event (const rapidjson::Value &, unsigned int channel, time_t date);
    };

  public:
    PVRFreeboxData (const std::string & path, int quality, int days, bool extended, int delay);
    virtual ~PVRFreeboxData ();

    // Freebox Server.
    std::string GetServer () const;

    // Quality setting.
    void SetQuality (int);
    // MaxDays setting.
    void SetDays (int);
    // Extended EPG.
    void SetExtended (bool);
    // Delay setting.
    void SetDelay (int);

    int       GetChannelsAmount ();
    PVR_ERROR GetChannels (ADDON_HANDLE, bool radio);
    int       GetChannelGroupsAmount ();
    PVR_ERROR GetChannelGroups (ADDON_HANDLE, bool radio);
    PVR_ERROR GetChannelGroupMembers (ADDON_HANDLE, const PVR_CHANNEL_GROUP &);
    PVR_ERROR GetChannelStreamProperties (const PVR_CHANNEL *, PVR_NAMED_VALUE *, unsigned int * count);

  protected:
    virtual void * Process ();

    // Process JSON channels.
    bool ProcessChannels ();

    // Process JSON EPG.
    void ProcessFull    (const rapidjson::Value & epg);
    void ProcessChannel (const rapidjson::Value & epg, unsigned int channel);
    void ProcessEvent   (const rapidjson::Value & epg, unsigned int channel, time_t, EPG_EVENT_STATE);

    // If /api/v5/tv/epg/programs/* queries had a "date", things would be *way* easier!
    void ProcessEvent   (const Event &, EPG_EVENT_STATE);

  protected:
    static bool ReadJSON (rapidjson::Document *, const std::string & url);

    template <typename T>
    inline static T JSON (const rapidjson::Value &);

    template <typename T>
    inline static T JSON (const rapidjson::Value &, const char * name, const T & value = T ());

  protected:
    // Full URL (protocol + server + query).
    std::string URL (const std::string & query) const;

  private:
    mutable P8PLATFORM::CMutex m_mutex;
    std::string m_path;
    std::string m_server;
    int m_delay;
    std::map<unsigned int, Channel> m_tv_channels;
    enum Quality m_tv_quality;
    std::queue<Query> m_epg_queries;
    std::set<std::string> m_epg_cache;
    int m_epg_days;
    time_t m_epg_last;
    bool m_epg_extended;
};

template <> inline bool        PVRFreeboxData::JSON<bool>        (const rapidjson::Value & json) {return json.GetBool   ();}
template <> inline int         PVRFreeboxData::JSON<int>         (const rapidjson::Value & json) {return json.GetInt    ();}
template <> inline std::string PVRFreeboxData::JSON<std::string> (const rapidjson::Value & json) {return json.GetString ();}

template <typename T>
T PVRFreeboxData::JSON (const rapidjson::Value & json, const char * name, const T & value)
{
  auto f = json.FindMember (name);
  return f != json.MemberEnd () ? JSON<T> (f->value) : value;
}

