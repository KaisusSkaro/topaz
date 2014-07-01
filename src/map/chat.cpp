/*
===========================================================================

Copyright (c) 2010-2014 Darkstar Dev Teams

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see http://www.gnu.org/licenses/

This file is part of DarkStar-server source code.

===========================================================================
*/

#include "chat.h"
#include "utils/zoneutils.h"
#include "utils/jailutils.h"
#include "entities/charentity.h"
#include "party.h"
#include "alliance.h"

#include "packets/message_standard.h"

namespace chat
{
	zmq::context_t zContext;
	zmq::socket_t* zSocket = NULL;

	void init(const char* chatIp, uint16 chatPort)
	{
		zContext = zmq::context_t(1);
		zSocket = new zmq::socket_t(zContext, ZMQ_DEALER);

		uint64 ipp = map_ip;
		uint64 port = map_port;
		ipp |= (port << 32);

		//if no ip/port were supplied, set to 1 (0 is not valid for an identity)
		if (ipp == 0)
		{
			ipp = 1;
		}

		zSocket->setsockopt(ZMQ_IDENTITY, &ipp, sizeof ipp);

		string_t server = "tcp://";
		server.append(chatIp);
		server.append(":");
		server.append(std::to_string(chatPort));

		try
		{
			zSocket->connect(server.c_str());
		}
		catch (zmq::error_t err)
		{
			ShowFatalError("Unable to connect chat socket: %s\n", err.what());
		}

		listen();
	}

	void listen()
	{
		while (true)
		{
			zmq::message_t type;
			zSocket->recv(&type);

			zmq::message_t extra;
			zSocket->recv(&extra);

			zmq::message_t packet;
			zSocket->recv(&packet);

			parse((CHATTYPE)RBUFB(type.data(),0), &extra, &packet);
		}
	}
	void parse(CHATTYPE type, zmq::message_t* extra, zmq::message_t* packet)
	{
		switch (type)
		{
			case CHAT_TELL:
			{
				CCharEntity* PChar = zoneutils::GetCharByName((int8*)extra->data()+4);
				if (PChar && PChar->status != STATUS_DISAPPEAR && !jailutils::InPrison(PChar))
				{
					if (PChar->nameflags.flags & FLAG_AWAY)
					{
						send(CHAT_MSG_DIRECT, extra->data(), sizeof uint32, new CMessageStandardPacket(PChar, 0, 0, 181));
					}
					else
					{
						CBasicPacket* newPacket = new CBasicPacket();
						memcpy(newPacket, packet->data(), packet->size());
						PChar->pushPacket(newPacket);
					}
				}
				else
				{
					send(CHAT_MSG_DIRECT, extra->data(), sizeof uint32, new CMessageStandardPacket(PChar, 0, 0, 125));
				}
				break;
			}
			case CHAT_PARTY:
			{
				uint32 charID = RBUFL(extra->data(), 0);
				CCharEntity* PChar = zoneutils::GetChar(RBUFL(extra->data(), 0));
				if (PChar)
				{
					if (PChar->PParty)
					{
						if (PChar->PParty->m_PAlliance != NULL)
						{
							for (uint8 i = 0; i < PChar->PParty->m_PAlliance->partyList.size(); ++i)
							{
								CBasicPacket* newPacket = new CBasicPacket();
								memcpy(newPacket, packet->data(), packet->size());
								((CParty*)PChar->PParty->m_PAlliance->partyList.at(i))->PushPacket(RBUFL(extra->data(), 4), 0, newPacket);
							}
						}
						else
						{
							CBasicPacket* newPacket = new CBasicPacket();
							memcpy(newPacket, packet->data(), packet->size());
							PChar->PParty->PushPacket(RBUFL(extra->data(), 4), 0, newPacket);
						}
					}
				}
				break;
			}
			case CHAT_LINKSHELL:
			{
				uint32 linkshellID = RBUFL(extra->data(), 0);
				CLinkshell* PLinkshell = linkshell::GetLinkshell(linkshellID);
				if (PLinkshell)
				{
					CBasicPacket* newPacket = new CBasicPacket();
					memcpy(newPacket, packet->data(), packet->size());
					PLinkshell->PushPacket(RBUFL(extra->data(), 4), newPacket);
				}
				break;
			}
			case CHAT_YELL:
			{
				zoneutils::ForEachZone([&packet](CZone* PZone)
				{
					if (PZone->CanUseMisc(MISC_YELL))
					{
						PZone->ForEachChar([&packet](CCharEntity* PChar)
						{
							CBasicPacket* newPacket = new CBasicPacket();
							memcpy(newPacket, packet->data(), packet->size());
							PChar->pushPacket(newPacket);
						});
					}
				});
				break;
			}
			case CHAT_SERVMES:
			{
				zoneutils::ForEachZone([&packet](CZone* PZone)
				{
					PZone->ForEachChar([&packet](CCharEntity* PChar)
					{
						CBasicPacket* newPacket = new CBasicPacket();
						memcpy(newPacket, packet->data(), packet->size());
						PChar->pushPacket(newPacket);
					});
				});
				break;
			}
			case CHAT_PT_INVITE:
			{
				uint32 id = RBUFL(extra->data(), 0);
				uint16 targid = RBUFW(extra->data(), 4);
				CCharEntity* PInvitee = zoneutils::GetCharFromWorld(id, targid);

				//make sure intvitee isn't dead or in jail, they aren't a party member and don't already have an invite pending, and your party is not full
				if (PInvitee->isDead() || jailutils::InPrison(PInvitee) || PInvitee->InvitePending.id != 0 || PInvitee->PParty != NULL)
				{
					send(CHAT_MSG_DIRECT, extra->data(), sizeof uint32, new CMessageStandardPacket(PInvitee, 0, 0, 23));
					return;
				}
				if (PInvitee->StatusEffectContainer->HasStatusEffect(EFFECT_LEVEL_SYNC))
				{
					send(CHAT_MSG_DIRECT, extra->data(), sizeof uint32, new CMessageStandardPacket(PInvitee, 0, 0, 236));
					return;
				}

				PInvitee->InvitePending.id = id;
				PInvitee->InvitePending.targid = targid;
				CBasicPacket* newPacket = new CBasicPacket();
				memcpy(newPacket, packet->data(), packet->size());
				PInvitee->pushPacket(newPacket);

				break;
			}
			case CHAT_MSG_DIRECT:
			{
				CCharEntity* PChar = zoneutils::GetChar(RBUFL(extra->data(),0));
				if (PChar)
				{
					CBasicPacket* newPacket = new CBasicPacket();
					memcpy(newPacket, packet->data(), packet->size());
					PChar->pushPacket(newPacket);
				}
				break;
			}
		}
	}

	void send(CHATTYPE type, void* data, size_t datalen, CBasicPacket* packet)
	{
		zmq::message_t newType(sizeof CHATTYPE);
		WBUFB(newType.data(),0) = type;
		zSocket->send(newType, ZMQ_SNDMORE);

		zmq::message_t newExtra(datalen);
		if (datalen > 0)
			memcpy(newExtra.data(), data, datalen);
		zSocket->send(newExtra, ZMQ_SNDMORE);

		zmq::message_t newPacket(packet, packet->getSize() * 2, [](void *data, void *hint){delete data; });
		zSocket->send(newPacket);
	}
};