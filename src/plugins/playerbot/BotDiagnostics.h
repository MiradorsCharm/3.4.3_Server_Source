/*
 * Playerbot AI - diagnostics.
 *
 * The stuck-bot watchdog. Once per second it asks a simple question with the
 * core's own numbers: is this bot's victim state actually making progress
 * (distance shrinking, victim health dropping, casts going off)? A bot that
 * has a victim for a while without any of those is reported once with one
 * compact line that contains exactly the facts the core will use on its next
 * swing/cast attempt - same metrics, no second opinion.
 */

#ifndef PLAYERBOT_BOT_DIAGNOSTICS_H
#define PLAYERBOT_BOT_DIAGNOSTICS_H

class BotAI;

class BotDiagnostics
{
public:
    /// called ~1x/second from BotAI::UpdateDiagnostics
    static void ReportStall(BotAI& ai);
};

#endif
