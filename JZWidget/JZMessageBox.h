#ifndef JZ_MESSAGE_BOX_H_
#define JZ_MESSAGE_BOX_H_

#include "JZDialog.h"

class JZMessageBox : public JZDialog
{
    JZ_OBJECT_DECLARE(JZMessageBox)

public:
    static JZMessageBox* information(JZWidget* parent, const std::string& title, const std::string& text);
    static JZMessageBox* warning(JZWidget* parent, const std::string& title, const std::string& text);
    static JZMessageBox* question(JZWidget* parent, const std::string& title, const std::string& text);

private:
    JZMessageBox(JZWidget* parent, const std::string& title, const std::string& text, bool hasCancel);

    void buildLayout(const std::string& title, const std::string& text, bool hasCancel);
};

#endif
