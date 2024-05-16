#include "App.hpp"
#include "Book.hpp"
#include "Librarydb.hpp"
#include "SQLiteCpp/Exception.h"
#include "User.hpp"

#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <exception>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

class Exit : public std::exception {};

int App::run() {
    try {
        login();
    }
    catch(const Exit& e){
        screen.Exit();
        // cleanup
    }
    catch (const SQLite::Exception& e) {
        screen.Exit();
        std::cerr<<"[ERROR] Database engine error. <"<<e.what()<<">"<<std::endl;
        return EXIT_FAILURE;
    }
    catch(const std::exception& e) {
        screen.Exit();
        std::cerr<<"[Error] Unknown error. <"<<e.what()<<">"<<std::endl;
        return EXIT_FAILURE;
    }
    //
    return EXIT_SUCCESS;
}

std::size_t readSession(const std::string& filename) {
    std::size_t session;
    std::ifstream ifs(filename);
    ifs>>session;
    ifs.close();
    return session;
}

void App::writeSessionFile(const std::size_t session) {
    auto ofs = std::ofstream(session_file);
    ofs<<session;
    ofs.close();
}

void App::attemptRestore() {
    auto usr = db->restoreSession(readSession(session_file));
    if( !usr){
        // restore failed, session expired
        return;
    }
    else{
        active_user = std::make_unique<User>(usr.value());
    }
}

void flip(bool& flag) {
    auto th = std::thread([&flag] {
        flag = true;
        std::this_thread::sleep_for(std::chrono::seconds{1});;
        flag = false;
    });
    th.detach();
}

void App::saveSession() {
    // Shouldn't be called when there is no active user
    if (not active_user)
        throw std::exception();

    std::size_t session = std::hash<std::string>{}(active_user->username);
    writeSessionFile(session);
    db->newSession(active_user->username, session);
}

void App::clearSession() {
    clearSessionFile();
    db->clearSession(active_user->username);
}

void App::clearSessionFile() {
    std::ofstream ofs(session_file, std::ios::trunc);
}

void App::login() {
    using namespace ftxui;

    if(newSession) {
        clearSessionFile();
    }
    else {
        attemptRestore();
        if (active_user) {
            home();
        }
    }

    bool signup_ok = false;
    std::string login_username, signup_username, signup_password, login_password, email, signup_error_message;
    auto signup_status = Renderer([&] {
        return text(signup_error_message) | color(Color::Red);
    }) | Maybe([&] {
            bool show_error = true;
            if ( !signup_username.empty() && signup_username.size() < 4) {
                signup_error_message = "Username too short!";
            }
            else if ( !signup_username.empty() && db->usernameExists(signup_username)) {
                signup_error_message = "Username is taken!";
            }
            else if ( !signup_password.empty() && signup_password.size() < 4) {
                signup_error_message = "Password is too short!";
            }
            else if (!email.empty() && db->emailIsUsed(email)) {
                signup_error_message = "Email is used before!";
            }
            else {
                show_error = false;
            }
            signup_ok = !(show_error || signup_username.empty() || signup_password.empty() || email.empty());
            return show_error;
        });

    bool show_failed_authentication = false;
    auto failed_login = Renderer([&] {
        return text("Login failed. Wrong credentials!") | color(Color::Red);
    }) | Maybe(&show_failed_authentication);

    auto signup_action = [&] {
        if (! signup_ok)
            return;

        // save sign-up info and set active_user
        auto usr = User{email, signup_username, UserClass::NORMAL};
        db->addUser(usr, signup_password);
        // signed up
        signup_password.clear();
        signup_username.clear();
        email.clear();
        active_user = std::make_unique<User>(usr);
        saveSession();
        home();
    };

    auto login_action = [&] {
        auto usr = db->authenticate(login_username, login_password);
        if (not usr) {
            login_username.clear();
            login_password.clear();
            flip(show_failed_authentication);
            }
        else {
            // loged in
            login_password.clear();
            login_username.clear();
            active_user = std::make_unique<User>(usr.value());
            saveSession();
            home();
        }
    };

    // input boxes
    ftxui::InputOption password_option;
    password_option.password = true;

    auto login_screen_container = Container::Vertical({
        Input(&login_username, "Username") | size(WIDTH, ftxui::EQUAL, 40) | border,
        Input(&login_password, "Password", password_option) | size(WIDTH, ftxui::EQUAL, 40) | border,
        failed_login,
        Container::Horizontal({
            Button("Login", login_action, ButtonOption::Ascii()),
            Button("Quit", [] { throw Exit(); }, ButtonOption::Ascii())
        })
    });

    auto signup_screen_container = Container::Vertical({
        Input(&email, "Email") | size(WIDTH, ftxui::EQUAL, 40) | border,
        Input(&signup_username, "Username") | size(WIDTH, ftxui::EQUAL, 40) | border,
        Input(&signup_password, "Password", password_option) | size(WIDTH, ftxui::EQUAL, 40) | ftxui::border,
        signup_status,
        Container::Horizontal({
            Button("Sign Up", signup_action, ButtonOption::Ascii()),
            Button("Quit", [&] { throw Exit(); }, ButtonOption::Ascii())
       })
    });

    std::vector<std::string> toggle_labels{"Login", "Signup"};
    int login_signup_selected = 0;

    auto login_signup_screen = Container::Vertical({
        Toggle(&toggle_labels, &login_signup_selected),
        Container::Tab({login_screen_container, signup_screen_container },
            &login_signup_selected)
    });


    auto login_signup_renderer = Renderer(login_signup_screen, [&] {
        return vbox({
            hbox(
                filler(),
                text("Welcome to Library Management System") | bold,
                filler()
            ),
            separator(),
            filler(),
            hbox(
                filler(),
                login_signup_screen->Render(),
                filler()
            ),
            filler()
        }) | border;
    });

    screen.Loop(login_signup_renderer);
}

void App::adminHome() {
    using namespace ftxui;
    std::string username = active_user->username;

    std::vector<std::string> main_selection {
        "Add a book",
        "Remove a book",
        "Remove a user",
        "Privelage management"
    };

    int main_menu_selected = 0;
    auto main_menu = Menu(&main_selection, &main_menu_selected);

    auto home_screen = Container::Vertical({
        Container::Horizontal({
            main_menu
        }),
        Button("Logout", [&]{
            clearSession();
            active_user = nullptr;
            screen.Exit();
        }, ButtonOption::Ascii()),
        Button("Quit", [&] { throw Exit(); }, ButtonOption::Ascii())
    });

    auto home_renderer = Renderer(home_screen, [&] {
        return ftxui::vbox(
            home_screen->Render()
        );
    });
    screen.Loop(home_screen);


}

void App::normalHome() {
    using namespace ftxui;
    std::string username = active_user->username;

    BookStack all_books = db->getAllBooks();
    BookStack borrowed = db->getBorrowed(username);
    BookStack favourites = db->getFavourites(username);

    std::vector<std::string> main_selection {
        "All books",
        "Borrowed",
        "Favourites",
        "My Account"
    };

    int main_menu_selected = 0;
    auto main_menu = Menu(&main_selection, &main_menu_selected);

    int all_book_selected = 0;
    auto all_book_menu = Container::Vertical({}, &all_book_selected);
    if (all_books.empty()) {
        all_book_menu->Add(
            Renderer([]{
                return vbox({
                    filler(),
                    text("You haven't borrowed any book yet"),
                    filler()
                    });
            })
        );
    }
    else {
        for(int i = 0; i<all_books.size(); ++i){
            all_book_menu->Add(MenuEntry(all_books[i].author + "_" + all_books[i].title));
        }
    }
    all_book_menu |= size(ftxui::WIDTH, ftxui::EQUAL, 60);

    int favourite_book_selected = 0;
    auto favourites_menu = Container::Vertical({}, &favourite_book_selected);
    if (favourites.empty()) {
        favourites_menu->Add(
            Renderer([]{
                return vbox({
                    filler(),
                    text("You haven't liked any book yet"),
                    filler()
                });
            }));
    }
    else {
        for(int i = 0; i<favourites.size(); ++i){
            favourites_menu->Add(MenuEntry(favourites[i].author + "_" + favourites[i].title));
        }
    }
   favourites_menu |= size(ftxui::WIDTH, ftxui::EQUAL, 60);

    int borrowed_book_selected = 0;
    auto borrowed_menu = Container::Vertical({}, &borrowed_book_selected);
    if (borrowed.empty()) {
        borrowed_menu->Add(
            Renderer([]{
                return vbox({
                    filler(),
                    text("No books in the library"),
                    filler()
                });
            }));
    }
    else {
        for(int i = 0; i<borrowed.size(); ++i) {
            borrowed_menu->Add(MenuEntry(borrowed[i].author + "_" + borrowed[i].title));
        }
    }
    borrowed_menu |= size(ftxui::WIDTH, ftxui::EQUAL, 60);

    auto main_tab = Container::Tab({
        Container::Horizontal({
            all_book_menu,
            Renderer([] { return separator(); }),
            bookDetail(all_books, all_book_selected)}),
        Container::Horizontal({
            borrowed_menu,
            Renderer([] { return separator(); }),
            bookDetail(borrowed, borrowed_book_selected)}),
        Container::Horizontal({
            favourites_menu,
            Renderer([] { return separator(); }),
            bookDetail(favourites, favourite_book_selected)}),
        Container::Vertical({
            Button("Change passoword", [] {}, ButtonOption::Ascii())
        })
    }, &main_menu_selected);

    auto main_menu_container = Container::Vertical({
        main_menu,
        Renderer([] { return filler(); }),
        Renderer([] { return separator(); }),
        Container::Horizontal({
            Button("Logout", [&]{
                clearSession();
                active_user = nullptr;
                screen.Exit();
            }, ButtonOption::Ascii()),
            Button("Quit", [&] { throw Exit(); }, ButtonOption::Ascii())
        })
    });

    auto home_screen = Container::Horizontal({
        main_menu_container,
        Renderer([] { return separator(); }),
        main_tab
    }) | border;

    screen.Loop(home_screen);
}

void App::home() {
    if(active_user->type == UserClass::NORMAL)
        normalHome();
    else {
        adminHome();
    }
}

ftxui::Component App::bookDetail(const BookStack& books, const int& selector) {
    using namespace ftxui;
    if (books.empty()) {
        return Container::Horizontal({
            Renderer([] { return filler(); }),
            Renderer([] { return text("Nothing selected"); }),
            Renderer([] { return filler(); }),
        });
    }

    return Container::Vertical({
        Renderer([&] {
            return text("Titile: " + books[selector].title);
        }),
        Renderer([&] {
            return text("Author: " + books[selector].author);
        }),
        Renderer([&] {
            return text("Publisher: " + books[selector].publisher);
        }) | Maybe([&] { return ! books[selector].publisher.empty(); }),
        Renderer([&] {
                return text("Publicatin Year: " + std::to_string(books[selector].pub_year));
        }) | Maybe([&] { return books[selector].pub_year > 0; }),
        Renderer([&] {
            return text("Edition: " + std::to_string(books[selector].edition));
        }) | Maybe([&] { return books[selector].edition > 0; }),
        Renderer([&] {
                return text("Rating: " + std::to_string(books[selector].rating));
            }) | Maybe([&] { return books[selector].rating > 0; }),
        Renderer([&] {
            return text("Availablity: " + std::string(books[selector].quantity > 0 ? "Available" : "Not Available"));
        }),
        Renderer([&] {
            return vbox({
                text("Description") | bold,
                paragraph(books[selector].description)
            });
        }) | Maybe([&] { return ! books[selector].description.empty(); })
    });
}
