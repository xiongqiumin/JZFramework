#ifndef JZ_DIALOG_H_
#define JZ_DIALOG_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZDialog : public JZWidget
{
    JZ_OBJECT_DECLARE(JZDialog)

public:
    enum Result { Rejected = 0, Accepted = 1 };

    explicit JZDialog(JZWidget* parent = nullptr);

    Result result() const;

    int exec();
    void accept();
    void reject();

jz_signals:
    JZSigVoid sigAccepted;
    JZSigVoid sigRejected;
    JZSigInt  sigFinished;  // Result

protected:
    void closeEvent(JZCloseEvent* event) override;

    Result m_result;
};

#endif
