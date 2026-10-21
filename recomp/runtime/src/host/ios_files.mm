// Ips3: iOS file access -- Documents, the log file, and the disc image picker.
//
// The 7.8 GB disc image is read in place: the picker hands out a security-scoped URL, which is
// kept as a bookmark so later launches open the same file without asking again.
#include "ios.h"

#include <SDL_syswm.h>

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstdio>

static NSString* const kBookmarkKey = @"Ips3GameImageBookmark";

std::filesystem::path ios::DocumentsPath()
{
    NSArray<NSURL*>* urls = [[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
                                                                   inDomains:NSUserDomainMask];
    return std::filesystem::path(urls.firstObject.path.UTF8String);
}

void ios::RedirectLog()
{
    const auto path = DocumentsPath() / "bayonetta-log.txt";
    freopen(path.c_str(), "w", stderr);
    freopen(path.c_str(), "a", stdout);
    setvbuf(stderr, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IOLBF, 0);
}

// Opens a bookmarked URL for reading; the access lasts for the life of the process.
static std::filesystem::path OpenBookmark(NSData* bookmark)
{
    BOOL stale = NO;
    NSError* error = nil;
    NSURL* url = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil
                               bookmarkDataIsStale:&stale error:&error];
    if (url == nil || ![url startAccessingSecurityScopedResource])
        return {};
    if (stale)
    {
        NSData* fresh = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        if (fresh != nil)
            [[NSUserDefaults standardUserDefaults] setObject:fresh forKey:kBookmarkKey];
    }
    return std::filesystem::path(url.path.UTF8String);
}

std::filesystem::path ios::FindGameImage()
{
    if (NSData* bookmark = [[NSUserDefaults standardUserDefaults] dataForKey:kBookmarkKey])
    {
        auto path = OpenBookmark(bookmark);
        if (!path.empty() && std::filesystem::exists(path))
            return path;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(DocumentsPath(), ec))
    {
        auto ext = entry.path().extension().string();
        for (auto& c : ext)
            c = char(tolower(c));
        if (ext == ".iso" && entry.is_regular_file(ec))
            return entry.path();
    }
    return {};
}

@interface Ips3PickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, copy) void (^done)(NSURL* url);
@end

@implementation Ips3PickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
    self.done(urls.firstObject);
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller
{
    self.done(nil);
}
@end

void ios::PickGameImage(SDL_Window* window, std::function<void(const std::filesystem::path&)> picked)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    UIViewController* root = nil;
    if (SDL_GetWindowWMInfo(window, &info))
        root = info.info.uikit.window.rootViewController;
    if (root == nil)
    {
        picked({});
        return;
    }

    UTType* iso = [UTType typeWithFilenameExtension:@"iso"];
    NSArray<UTType*>* types = iso ? @[ iso, UTTypeData ] : @[ UTTypeData ];
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO];

    static Ips3PickerDelegate* delegate; // the picker holds its delegate weakly
    delegate = [Ips3PickerDelegate new];
    auto callback = std::make_shared<std::function<void(const std::filesystem::path&)>>(std::move(picked));
    delegate.done = ^(NSURL* url) {
        std::filesystem::path path;
        if (url != nil)
        {
            if ([url startAccessingSecurityScopedResource])
            {
                NSData* bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil
                                                  relativeToURL:nil error:nil];
                if (bookmark != nil)
                    [[NSUserDefaults standardUserDefaults] setObject:bookmark forKey:kBookmarkKey];
            }
            path = url.path.UTF8String;
        }
        (*callback)(path);
    };
    picker.delegate = delegate;
    picker.allowsMultipleSelection = NO;
    [root presentViewController:picker animated:YES completion:nil];
}
