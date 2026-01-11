138 stdcall @(long str str str str long str long long str str long) USERENV_138

@ stdcall CreateAppContainerProfile(wstr wstr wstr ptr long ptr)
@ stdcall CreateEnvironmentBlock(ptr ptr long)
# @ stub CreateProfile
@ stdcall DeleteAppContainerProfile(wstr)
# @ stub DeleteProfileA
# @ stub DeleteProfileW
@ stdcall DeriveAppContainerSidFromAppContainerName(wstr ptr)
# @ stub DeriveRestrictedAppContainerSidFromAppContainerSidAndRestrictedName
@ stdcall DestroyEnvironmentBlock(ptr)
@ stdcall EnterCriticalPolicySection(long)
@ stdcall ExpandEnvironmentStringsForUserA(ptr str ptr long)
@ stdcall ExpandEnvironmentStringsForUserW(ptr wstr ptr long)
# @ stub FreeGPOListA
# @ stub FreeGPOListW
@ stdcall GetAllUsersProfileDirectoryA(ptr ptr)
@ stdcall GetAllUsersProfileDirectoryW(ptr ptr)
@ stdcall GetAppContainerFolderPath(wstr ptr)
@ stdcall GetAppContainerRegistryLocation(long ptr)
# @ stub GetAppliedGPOListA
@ stdcall GetAppliedGPOListW(long wstr ptr ptr ptr)
@ stdcall GetDefaultUserProfileDirectoryA(ptr ptr)
@ stdcall GetDefaultUserProfileDirectoryW(ptr ptr)
# @ stub GetGPOListA
# @ stub GetGPOListW
@ stdcall GetProfilesDirectoryA(ptr ptr)
@ stdcall GetProfilesDirectoryW(ptr ptr)
@ stdcall GetProfileType(ptr)
@ stdcall GetUserProfileDirectoryA(ptr ptr ptr)
@ stdcall GetUserProfileDirectoryW(ptr ptr ptr)
@ stdcall LeaveCriticalPolicySection(ptr)
@ stdcall LoadUserProfileA(ptr ptr)
@ stdcall LoadUserProfileW(ptr ptr)
# @ stub ProcessGroupPolicyCompleted
# @ stub ProcessGroupPolicyCompletedEx
# @ stub RefreshPolicy
# @ stub RefreshPolicyEx
@ stdcall RegisterGPNotification(long long)
# @ stub RsopAccessCheckByType
# @ stub RsopFileAccessCheck
# @ stub RsopResetPolicySettingStatus
# @ stub RsopSetPolicySettingStatus
@ stdcall UnloadUserProfile(ptr ptr)
@ stdcall UnregisterGPNotification(long)
