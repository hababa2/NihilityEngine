#pragma once

#include "Defines.hpp"

#include "Containers\String.hpp"

#include "Discord\discord.h"

struct NH_API Activity
{
	StringView name = "";
	StringView details = "";
	StringView state = "";
	StringView largeImage = "";
	StringView largeImageText = "";
	StringView smallImage = "";
	StringView smallImageText = "";
};

class NH_API Discord
{
public:
	static void SetActivity(const Activity& activity);
	static void ChangeActivityName(const StringView& name);
	static void ChangeActivityDetails(const StringView& details);
	static void ChangeActivityState(const StringView& state);
	static void ChangeActivityLargeImage(const StringView& largeImage);
	static void ChangeActivityLargeImageText(const StringView& largeImageText);
	static void ChangeActivitySmallImage(const StringView& smallImage);
	static void ChangeActivitySmallImageText(const StringView& smallImageText);

private:
	static void Initialize(U64 discordAppId);
	static void Shutdown();
	static void Update();

	static bool TryCreateCore();

	static void Log(discord::LogLevel level, const char* message);
	static void Callback(discord::Result result);

	static discord::User currentUser;
	static discord::Core* core;
	static discord::Activity activity;
	static U64 discordAppId;

	friend class Engine;
};