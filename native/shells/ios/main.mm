#import <UIKit/UIKit.h>

#include "bridge_native/client.hpp"

#include <cstdlib>

@interface BridgeNativeIOS : UIViewController
@property(nonatomic, strong) UILabel* status;
@end

@implementation BridgeNativeIOS
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.status = [[UILabel alloc] initWithFrame:CGRectMake(20, 100, 350, 300)];
    self.status.numberOfLines = 0; self.status.text = @"BridgePanel settings come from the app's managed configuration.";
    [self.view addSubview:self.status];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const char* url = std::getenv("BRIDGEPANEL_URL"); const char* token = std::getenv("BRIDGEPANEL_TOKEN"); const char* machine = std::getenv("BRIDGEPANEL_MACHINE");
        bridge_native::BridgePanelClient client({url ? url : "", token ? token : "", {5000, true}});
        auto capabilities = client.capabilities();
        auto peers = client.peers();
        auto sessions = client.sessions(machine ? machine : "");
        std::string text;
        if (capabilities.ok() && peers.ok() && sessions.ok())
            text = bridge_native::Json{{"capabilities", capabilities.value()}, {"peers", peers.value()}, {"sessions", sessions.value()}}.dump(2);
        else {
            const auto& error = !capabilities.ok() ? capabilities.error() : (!peers.ok() ? peers.error() : sessions.error());
            text = error.code + ": " + error.message;
        }
        dispatch_async(dispatch_get_main_queue(), ^{ self.status.text = [NSString stringWithUTF8String:text.c_str()]; });
    });
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds]; self.window.rootViewController = [BridgeNativeIOS new]; [self.window makeKeyAndVisible]; return YES;
}
@end

int main(int argc, char* argv[]) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class])); } }
