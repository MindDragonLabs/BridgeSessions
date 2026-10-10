#import <AppKit/AppKit.h>

#include "bridge_native/client.hpp"

#include <dispatch/dispatch.h>
#include <cstdlib>
#include <string>
#include <utility>

namespace {
std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

std::string field_value(NSTextField* field) {
    const char* value = [[field stringValue] UTF8String];
    return value ? value : "";
}
}

@interface BridgeNativeApp : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSTextField* status;
@property(nonatomic, strong) NSTextField* machine;
@property(nonatomic, strong) NSTextField* session;
@property(nonatomic, strong) NSTextField* command;
@property(nonatomic, strong) NSTextField* input;
- (void)showStatusText:(NSString*)text;
@end

@implementation BridgeNativeApp

- (bridge_native::BridgePanelClient)client {
    bridge_native::ClientOptions options;
    options.base_url = env_value("BRIDGEPANEL_URL");
    options.bearer_token = env_value("BRIDGEPANEL_TOKEN");
    options.http.timeout_ms = 5000;
    return bridge_native::BridgePanelClient(std::move(options));
}

- (void)showStatusText:(NSString*)text {
    dispatch_async(dispatch_get_main_queue(), ^{ self.status.stringValue = text; });
}

- (void)refresh:(id)sender {
    const std::string machine = field_value(self.machine);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto client = [self client];
        auto capabilities = client.capabilities();
        auto peers = client.peers();
        auto sessions = client.sessions(machine);
        if (capabilities.ok() && peers.ok() && sessions.ok()) {
            bridge_native::Json result{{"capabilities", capabilities.value()}, {"peers", peers.value()}, {"sessions", sessions.value()}};
            [self showStatusText:[NSString stringWithUTF8String:result.dump(2).c_str()]];
        } else {
            const auto& error = !capabilities.ok() ? capabilities.error() : (!peers.ok() ? peers.error() : sessions.error());
            [self showStatusText:[NSString stringWithFormat:@"%s: %s", error.code.c_str(), error.message.c_str()]];
        }
    });
}

- (void)createSession:(id)sender {
    const std::string machine = field_value(self.machine), name = field_value(self.session), command = field_value(self.command);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto client = [self client];
        auto result = client.create_session(machine, name, command, 80, 24);
        [self showStatusText:result.ok() ? [NSString stringWithUTF8String:result.value().dump().c_str()] : [NSString stringWithFormat:@"%s: %s", result.error().code.c_str(), result.error().message.c_str()]];
    });
}

- (void)sendInput:(id)sender {
    const std::string machine = field_value(self.machine), name = field_value(self.session), input = field_value(self.input);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto client = [self client];
        auto result = client.send_input(machine, name, input);
        [self showStatusText:result.ok() ? @"input accepted" : [NSString stringWithFormat:@"%s: %s", result.error().code.c_str(), result.error().message.c_str()]];
    });
}

- (void)killSession:(id)sender {
    const std::string machine = field_value(self.machine), name = field_value(self.session);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto client = [self client];
        auto result = client.kill_session(machine, name);
        [self showStatusText:result.ok() ? @"session termination accepted" : [NSString stringWithFormat:@"%s: %s", result.error().code.c_str(), result.error().message.c_str()]];
    });
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 700, 420)
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
    [self.window center];
    self.window.title = @"BridgeSessions Native";
    NSView* view = self.window.contentView;
    self.machine = [NSTextField textFieldWithString:[NSString stringWithUTF8String:env_value("BRIDGEPANEL_MACHINE").c_str()]]; self.machine.frame = NSMakeRect(20, 365, 300, 24);
    self.session = [NSTextField textFieldWithString:@"native-session"]; self.session.frame = NSMakeRect(20, 330, 300, 24);
    self.command = [NSTextField textFieldWithString:@"/bin/sh"]; self.command.frame = NSMakeRect(20, 295, 300, 24);
    self.input = [NSTextField textFieldWithString:@""]; self.input.frame = NSMakeRect(20, 260, 300, 24);
    [view addSubview:self.machine]; [view addSubview:self.session]; [view addSubview:self.command]; [view addSubview:self.input];
    NSArray* titles = @[@"Refresh peers/sessions", @"Create session", @"Send input", @"Kill session"];
    NSArray* actions = @[@"refresh:", @"createSession:", @"sendInput:", @"killSession:"];
    for (NSUInteger i = 0; i < titles.count; ++i) {
        NSButton* button = [NSButton buttonWithTitle:titles[i] target:self action:NSSelectorFromString(actions[i])];
        button.frame = NSMakeRect(340, 365 - static_cast<CGFloat>(i) * 40, 260, 28);
        [view addSubview:button];
    }
    self.status = [NSTextField labelWithString:@"Enter BridgePanel settings or use BRIDGEPANEL_URL/TOKEN/MACHINE."];
    self.status.frame = NSMakeRect(20, 20, 650, 210); self.status.lineBreakMode = NSLineBreakByWordWrapping;
    [view addSubview:self.status];
    [self.window makeKeyAndOrderFront:nil];
}
@end

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* application = [NSApplication sharedApplication];
        BridgeNativeApp* delegate = [BridgeNativeApp new];
        application.delegate = delegate;
        [application run];
    }
    return 0;
}
