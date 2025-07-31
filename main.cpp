#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QDirIterator>
#include <QSettings>
#include <QFileInfo>
#include <QPixmap>
#include <QPalette>
#include <QDebug>
#include <QScreen>
#include <QSvgRenderer>
#include <QPainter>
#include <QImageReader>
#include <QMessageBox>
#include <iostream>

#include <security/pam_appl.h>
#include <string.h>
#include <pwd.h>

#include <systemd/sd-bus.h>

QPixmap loadBackgroundPixmap(const QString &directory = "/usr/share/desktop-base/homeworld-theme/wallpaper/contents/images/") {
    QDirIterator it(directory, QStringList() << "*.jpg" << "*.png" << "*.svg", QDir::Files);
    while (it.hasNext()) {
        QString path = it.next();
        QFileInfo fileInfo(path);

        if (fileInfo.suffix().toLower() == "svg") {
            QSvgRenderer svg(path);
            QSize screenSize = QGuiApplication::primaryScreen()->size();
            QPixmap pixmap(screenSize);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            svg.render(&painter);
            return pixmap;
        } else {
            QPixmap pix(path);
            if (!pix.isNull()) {
                return pix;
            }
        }
    }
    return QPixmap();
}

void populateSessions(QComboBox *comboBox, const QString &directory = "/usr/share/wayland-sessions/") {
    QDirIterator it(directory, QStringList("*.desktop"), QDir::Files);
    while (it.hasNext()) {
        QString path = it.next();
        QSettings desktopFile(path, QSettings::IniFormat);

        QString name = desktopFile.value("Desktop Entry/Name").toString();
        QString exec = desktopFile.value("Desktop Entry/Exec").toString();

        if (name.isEmpty()) {
            QFileInfo info(path);
            name = info.baseName();
        }

        if (!exec.isEmpty()) {
            comboBox->addItem(name);
            comboBox->setItemData(comboBox->count() - 1, exec);
        }
    }
}

struct PamContext {
    QString password;
};

int pam_conv_handler(int num_msg, const struct pam_message **msg,
                     struct pam_response **resp, void *appdata_ptr) {
    if (!appdata_ptr || num_msg <= 0) return PAM_CONV_ERR;

    PamContext *ctx = reinterpret_cast<PamContext *>(appdata_ptr);
    *resp = (struct pam_response *)calloc(num_msg, sizeof(struct pam_response));
    if (!*resp) return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; ++i) {
        switch (msg[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF:  // password
                (*resp)[i].resp = strdup(ctx->password.toUtf8().constData());
                break;
            case PAM_PROMPT_ECHO_ON:   // username input (optional)
                (*resp)[i].resp = strdup("");
                break;
            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO:
                (*resp)[i].resp = nullptr;
                break;
            default:
                free(*resp);
                *resp = nullptr;
                return PAM_CONV_ERR;
        }
    }

    return PAM_SUCCESS;
}

bool authenticateWithPam(const QString &username, const QString &password) {
    pam_handle_t *pamh = nullptr;
    PamContext ctx{ password };

    struct pam_conv conv = {
        pam_conv_handler,
        &ctx
    };

    int ret = pam_start("login", username.toUtf8().constData(), &conv, &pamh);
    if (ret != PAM_SUCCESS) return false;

    ret = pam_authenticate(pamh, 0);
    if (ret != PAM_SUCCESS) {
        pam_end(pamh, ret);
        return false;
    }

    ret = pam_acct_mgmt(pamh, 0);
    if (ret != PAM_SUCCESS) {
        pam_end(pamh, ret);
        return false;
    }

    pam_end(pamh, PAM_SUCCESS);
    return true;
}

bool createSessionWithLogind(const std::string& username) {
    struct passwd* pw = getpwnam(username.c_str());
    if (!pw) {
        std::cerr << "No such user: " << username << "\n";
        return false;
    }

    uid_t uid = pw->pw_uid;
    pid_t pid = getpid();

    sd_bus* bus = nullptr;
    sd_bus_message* m = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        std::cerr << "Failed to open system bus: " << strerror(-r) << "\n";
        return false;
    }

    r = sd_bus_message_new_method_call(
        bus,
        &m,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "CreateSession"
    );
    if (r < 0) {
        std::cerr << "Failed to create method call: " << strerror(-r) << "\n";
        return false;
    }

    // Append arguments according to uusssssussbss + a(sv)
    r = sd_bus_message_append(m, "uusssssussbss",
        (uint32_t)uid,          // uid
        (uint32_t)pid,          // pid
        "wl_greeter",           // service
        "wayland",              // type
        "greeter",              // class
        "",                     // desktop
        "seat0",                // seat
        1,                      // vtnr
        "",                     // tty
        "",                     // display
        0,                      // remote
        "",                     // remote user
        ""                      // remote host
        ""                      // remote hostname
    );
    if (r < 0) {
        std::cerr << "Failed to append base args: " << strerror(-r) << "\n";
        return false;
    }

    // Open and close an empty a(sv) array
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(sv)");
    if (r < 0) {
        std::cerr << "Failed to open a(sv) container: " << strerror(-r) << "\n";
        return false;
    }

    // No properties for now

    r = sd_bus_message_close_container(m);
    if (r < 0) {
        std::cerr << "Failed to close a(sv) container: " << strerror(-r) << "\n";
        return false;
    }

    // Send the message
    r = sd_bus_call(bus, m, 0, &error, &reply);
    if (r < 0) {
        std::cerr << "CreateSession failed: " << error.message << "\n";
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }

    const char* session_id = nullptr;
    const char* session_path = nullptr;

    r = sd_bus_message_read(reply, "so", &session_id, &session_path);
    if (r < 0) {
        std::cerr << "Failed to parse reply: " << strerror(-r) << "\n";
        return false;
    }

    std::cout << "Session created successfully:\n";
    std::cout << "  ID: " << session_id << "\n";
    std::cout << "  Path: " << session_path << "\n";

    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return true;
}


int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // Main window
    QWidget window;
    window.setWindowTitle("Wayland Greeter");

    // Load background image (supports JPG, PNG, SVG)
	QPixmap bgPixmap = loadBackgroundPixmap();
	if (!bgPixmap.isNull()) {
		QPalette palette;
		palette.setBrush(QPalette::Window, bgPixmap.scaled(
			app.primaryScreen()->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
		window.setAutoFillBackground(true);
		window.setPalette(palette);
	} else {
		qWarning() << "No background image found or failed to load.";
	}

    // Login form container
    QWidget *loginPanel = new QWidget(&window);
    QVBoxLayout *mainLayout = new QVBoxLayout(loginPanel);
    loginPanel->setFixedWidth(400);
    loginPanel->setStyleSheet("background-color: rgba(0, 0, 0, 0.6); color: white; padding: 20px; border-radius: 10px;");

    // Username
    QLabel *userLabel = new QLabel("Username:");
    QLineEdit *userInput = new QLineEdit();
    mainLayout->addWidget(userLabel);
    mainLayout->addWidget(userInput);

    // Password
    QLabel *passLabel = new QLabel("Password:");
    QLineEdit *passInput = new QLineEdit();
    passInput->setEchoMode(QLineEdit::Password);
    mainLayout->addWidget(passLabel);
    mainLayout->addWidget(passInput);

    // Session
    QLabel *sessionLabel = new QLabel("Session:");
    QComboBox *sessionSelect = new QComboBox();
    populateSessions(sessionSelect);
    mainLayout->addWidget(sessionLabel);
    mainLayout->addWidget(sessionSelect);

    // Login button
    QPushButton *loginButton = new QPushButton("Login");
    mainLayout->addWidget(loginButton);

	QObject::connect(passInput, &QLineEdit::returnPressed, loginButton, &QPushButton::click);
	QObject::connect(userInput, &QLineEdit::returnPressed, loginButton, &QPushButton::click);

    QObject::connect(loginButton, &QPushButton::clicked, [&]() {
        QString username = userInput->text();
		QString password = passInput->text();
		QString sessionName = sessionSelect->currentText();
		QString execCommand = sessionSelect->currentData().toString();

		if (authenticateWithPam(username, password)) {
			std::cout << "Authentication successful for " << username.toStdString() << std::endl;
			// QMessageBox::warning(&window, "Login Ok", "Valid shit.");
			if(createSessionWithLogind(username.toStdString())) {
				QMessageBox::warning(&window, "Login Ok", "Valid shit.");
			} else {
				QMessageBox::warning(&window, "Login Ok", "Valid shit but logind doesn't approve!.");
			}
			// Next: call logind via sd-bus and start session
		} else {
			std::cerr << "Authentication failed for " << username.toStdString() << std::endl;
			QMessageBox::warning(&window, "Login Failed", "Invalid username or password.");
			passInput->clear();
		}
    });

    // Center the login panel
    QHBoxLayout *outerLayout = new QHBoxLayout(&window);
    outerLayout->addStretch();
    outerLayout->addWidget(loginPanel);
    outerLayout->addStretch();

    window.setLayout(outerLayout);
    window.showFullScreen();

    return app.exec();
}
